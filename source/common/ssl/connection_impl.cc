#include "common/ssl/connection_impl.h"

#include <cstdint>
#include <string>
#include <vector>

#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/hex.h"
#include "common/network/utility.h"

#include "openssl/err.h"
#include "openssl/x509v3.h"

namespace Envoy {
namespace Ssl {

namespace {
// TODO(mattklein123): Currently we don't populate local address for client connections. Nothing
// looks at this currently, but we may want to populate this later for logging purposes.
Network::Address::InstanceConstSharedPtr
getNullLocalAddress(const Network::Address::Instance& address) {
  if (address.type() == Network::Address::Type::Ip &&
      address.ip()->version() == Network::Address::IpVersion::v6) {
    return Network::Utility::getIpv6AnyAddress();
  }
  // Default to IPv4 any address.
  return Network::Utility::getIpv4AnyAddress();
}
} // namespace

ConnectionImpl::ConnectionImpl(Event::DispatcherImpl& dispatcher, int fd,
                               Network::Address::InstanceConstSharedPtr remote_address,
                               Network::Address::InstanceConstSharedPtr local_address,
                               Network::Address::InstanceConstSharedPtr bind_to_address,
                               bool using_original_dst, bool connected, Context& ctx,
                               InitialState state)
    : Network::ConnectionImpl(dispatcher, fd, remote_address, local_address, bind_to_address,
                              using_original_dst, connected),
      ctx_(dynamic_cast<Ssl::ContextImpl&>(ctx)), ssl_(ctx_.newSsl()) {
  BIO* bio = BIO_new_socket(fd, 0);
  SSL_set_bio(ssl_.get(), bio, bio);

  SSL_set_mode(ssl_.get(), SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (state == InitialState::Client) {
    SSL_set_connect_state(ssl_.get());
  } else {
    ASSERT(state == InitialState::Server);
    SSL_set_accept_state(ssl_.get());
  }
}

ConnectionImpl::~ConnectionImpl() {
  // Filters may care about whether this connection is an SSL connection or not in their
  // destructors for stat reasons. We destroy the filters here vs. the base class destructors
  // to make sure they have the chance to still inspect SSL specific data via virtual functions.
  filter_manager_.destroyFilters();
}

Network::ConnectionImpl::IoResult ConnectionImpl::doReadFromSocket() {
  if (!handshake_complete_) {
    PostIoAction action = doHandshake();
    if (action == PostIoAction::Close || !handshake_complete_) {
      return {action, 0};
    }
  }

  bool keep_reading = true;
  PostIoAction action = PostIoAction::KeepOpen;
  uint64_t bytes_read = 0;
  while (keep_reading) {
    // We use 2 slices here so that we can use the remainder of an existing buffer chain element
    // if there is extra space. 16K read is arbitrary and can be tuned later.
    Buffer::RawSlice slices[2];
    uint64_t slices_to_commit = 0;
    uint64_t num_slices = read_buffer_.reserve(16384, slices, 2);
    for (uint64_t i = 0; i < num_slices; i++) {
      int rc = SSL_read(ssl_.get(), slices[i].mem_, slices[i].len_);
      ENVOY_CONN_LOG(trace, "ssl read returns: {}", *this, rc);
      if (rc > 0) {
        slices[i].len_ = rc;
        slices_to_commit++;
        bytes_read += rc;
      } else {
        keep_reading = false;
        int err = SSL_get_error(ssl_.get(), rc);
        switch (err) {
        case SSL_ERROR_WANT_READ:
          break;
        case SSL_ERROR_WANT_WRITE:
        // Renegotiation has started. We don't handle renegotiation so just fall through.
        default:
          drainErrorQueue();
          action = PostIoAction::Close;
          break;
        }

        break;
      }
    }

    if (slices_to_commit > 0) {
      read_buffer_.commit(slices, slices_to_commit);
      if (shouldDrainReadBuffer()) {
        setReadBufferReady();
        keep_reading = false;
      }
    }
  }

  return {action, bytes_read};
}

Network::ConnectionImpl::PostIoAction ConnectionImpl::doHandshake() {
  ASSERT(!handshake_complete_);
  int rc = SSL_do_handshake(ssl_.get());
  if (rc == 1) {
    ENVOY_CONN_LOG(debug, "handshake complete", *this);
    handshake_complete_ = true;
    ctx_.logHandshake(ssl_.get());
    raiseEvent(Network::ConnectionEvent::Connected);

    // It's possible that we closed during the handshake callback.
    return state() == State::Open ? PostIoAction::KeepOpen : PostIoAction::Close;
  } else {
    int err = SSL_get_error(ssl_.get(), rc);
    ENVOY_CONN_LOG(debug, "handshake error: {}", *this, err);
    switch (err) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return PostIoAction::KeepOpen;
    default:
      drainErrorQueue();
      return PostIoAction::Close;
    }
  }
}

void ConnectionImpl::drainErrorQueue() {
  bool saw_error = false;
  bool saw_counted_error = false;
  while (uint64_t err = ERR_get_error()) {
    if (ERR_GET_LIB(err) == ERR_LIB_SSL) {
      if (ERR_GET_REASON(err) == SSL_R_PEER_DID_NOT_RETURN_A_CERTIFICATE) {
        ctx_.stats().fail_verify_no_cert_.inc();
        saw_counted_error = true;
      } else if (ERR_GET_REASON(err) == SSL_R_CERTIFICATE_VERIFY_FAILED) {
        saw_counted_error = true;
      }
    }
    saw_error = true;

    ENVOY_CONN_LOG(debug, "SSL error: {}:{}:{}:{}", *this, err, ERR_lib_error_string(err),
                   ERR_func_error_string(err), ERR_reason_error_string(err));
    UNREFERENCED_PARAMETER(err);
  }
  if (saw_error && !saw_counted_error) {
    ctx_.stats().connection_error_.inc();
  }
}

Network::ConnectionImpl::IoResult ConnectionImpl::doWriteToSocket() {
  if (!handshake_complete_) {
    PostIoAction action = doHandshake();
    if (action == PostIoAction::Close || !handshake_complete_) {
      return {action, 0};
    }
  }

  uint64_t original_buffer_length = write_buffer_->length();
  uint64_t total_bytes_written = 0;
  bool keep_writing = true;
  while ((original_buffer_length != total_bytes_written) && keep_writing) {
    // Protect against stack overflow if the buffer has a very large buffer chain.
    // TODO(mattklein123): See the comment on getRawSlices() for why we have to also check
    // original_buffer_length != total_bytes_written during loop iteration.
    // TODO(mattklein123): As it relates to our fairness efforts, we might want to limit the number
    // of iterations of this loop, either by pure iterations, bytes written, etc.
    const uint64_t MAX_SLICES = 32;
    Buffer::RawSlice slices[MAX_SLICES];
    uint64_t num_slices = write_buffer_->getRawSlices(slices, MAX_SLICES);

    uint64_t inner_bytes_written = 0;
    for (uint64_t i = 0; (i < num_slices) && (original_buffer_length != total_bytes_written); i++) {
      // SSL_write() requires that if a previous call returns SSL_ERROR_WANT_WRITE, we need to call
      // it again with the same parameters. Most implementations keep track of the last write size.
      // In our case we don't need to do that because: a) SSL_write() will not write partial
      // buffers. b) We only move() into the write buffer, which means that it's impossible for a
      // particular chain to increase in size. So as long as we start writing where we left off we
      // are guaranteed to call SSL_write() with the same parameters.
      int rc = SSL_write(ssl_.get(), slices[i].mem_, slices[i].len_);
      ENVOY_CONN_LOG(trace, "ssl write returns: {}", *this, rc);
      if (rc > 0) {
        inner_bytes_written += rc;
        total_bytes_written += rc;
      } else {
        int err = SSL_get_error(ssl_.get(), rc);
        switch (err) {
        case SSL_ERROR_WANT_WRITE:
          keep_writing = false;
          break;
        case SSL_ERROR_WANT_READ:
        // Renegotiation has started. We don't handle renegotiation so just fall through.
        default:
          drainErrorQueue();
          return {PostIoAction::Close, total_bytes_written};
        }

        break;
      }
    }

    // Draining must be done within the inner loop, otherwise we will keep getting the same slices
    // at the beginning of the buffer.
    if (inner_bytes_written > 0) {
      write_buffer_->drain(inner_bytes_written);
    }
  }

  return {PostIoAction::KeepOpen, total_bytes_written};
}

void ConnectionImpl::onConnected() { ASSERT(!handshake_complete_); }

bool ConnectionImpl::peerCertificatePresented() {
  bssl::UniquePtr<X509> cert(SSL_get_peer_certificate(ssl_.get()));
  return cert != nullptr;
}

std::string ConnectionImpl::uriSanLocalCertificate() {
  // The cert object is not owned.
  X509* cert = SSL_get_certificate(ssl_.get());
  if (!cert) {
    return "";
  }
  return getUriSanFromCertificate(cert);
}

std::string ConnectionImpl::sha256PeerCertificateDigest() {
  bssl::UniquePtr<X509> cert(SSL_get_peer_certificate(ssl_.get()));
  if (!cert) {
    return "";
  }

  std::vector<uint8_t> computed_hash(SHA256_DIGEST_LENGTH);
  unsigned int n;
  X509_digest(cert.get(), EVP_sha256(), computed_hash.data(), &n);
  RELEASE_ASSERT(n == computed_hash.size());
  return Hex::encode(computed_hash);
}

std::string ConnectionImpl::subjectPeerCertificate() {
  bssl::UniquePtr<X509> cert(SSL_get_peer_certificate(ssl_.get()));
  if (!cert) {
    return "";
  }

  bssl::UniquePtr<BIO> buf(BIO_new(BIO_s_mem()));
  RELEASE_ASSERT(buf != nullptr);

  // flags=XN_FLAG_RFC2253 is the documented parameter for single-line output in RFC 2253 format.
  X509_NAME_print_ex(buf.get(), X509_get_subject_name(cert.get()), 0 /* indent */, XN_FLAG_RFC2253);

  const uint8_t* data;
  size_t data_len;
  int rc = BIO_mem_contents(buf.get(), &data, &data_len);
  ASSERT(rc == 1);
  UNREFERENCED_PARAMETER(rc);
  return std::string(reinterpret_cast<const char*>(data), data_len);
}

std::string ConnectionImpl::uriSanPeerCertificate() {
  bssl::UniquePtr<X509> cert(SSL_get_peer_certificate(ssl_.get()));
  if (!cert) {
    return "";
  }
  return getUriSanFromCertificate(cert.get());
}

std::string ConnectionImpl::getUriSanFromCertificate(X509* cert) {
  STACK_OF(GENERAL_NAME)* altnames = static_cast<STACK_OF(GENERAL_NAME)*>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));

  if (altnames == nullptr) {
    return "";
  }

  std::string result;
  int n = sk_GENERAL_NAME_num(altnames);
  if (n > 0) {
    // Only take the first item in altnames since we only set one uri in cert.
    GENERAL_NAME* altname = sk_GENERAL_NAME_value(altnames, 0);
    switch (altname->type) {
    case GEN_URI:
      result.append(
          reinterpret_cast<const char*>(ASN1_STRING_data(altname->d.uniformResourceIdentifier)));
      break;
    default:
      // Default to empty;
      break;
    }
  }

  sk_GENERAL_NAME_pop_free(altnames, GENERAL_NAME_free);
  return result;
}

ClientConnectionImpl::ClientConnectionImpl(Event::DispatcherImpl& dispatcher, Context& ctx,
                                           Network::Address::InstanceConstSharedPtr address,
                                           Network::Address::InstanceConstSharedPtr source_address)
    : ConnectionImpl(dispatcher, address->socket(Network::Address::SocketType::Stream), address,
                     getNullLocalAddress(*address), source_address, false, false, ctx,
                     InitialState::Client) {}

void ClientConnectionImpl::connect() { doConnect(); }

void ConnectionImpl::closeSocket(Network::ConnectionEvent close_type) {
  if (handshake_complete_ && state() != State::Closed) {
    // Attempt to send a shutdown before closing the socket. It's possible this won't go out if
    // there is no room on the socket. We can extend the state machine to handle this at some point
    // if needed.
    int rc = SSL_shutdown(ssl_.get());
    ENVOY_CONN_LOG(debug, "SSL shutdown: rc={}", *this, rc);
    UNREFERENCED_PARAMETER(rc);
    drainErrorQueue();
  }

  Network::ConnectionImpl::closeSocket(close_type);
}

std::string ConnectionImpl::nextProtocol() const {
  const unsigned char* proto;
  unsigned int proto_len;
  SSL_get0_alpn_selected(ssl_.get(), &proto, &proto_len);
  return std::string(reinterpret_cast<const char*>(proto), proto_len);
}

} // namespace Ssl
} // namespace Envoy
