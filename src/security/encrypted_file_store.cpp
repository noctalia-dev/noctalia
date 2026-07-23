#include "security/encrypted_file_store.h"

#include "config/atomic_file.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <sodium.h>
#include <string_view>
#include <system_error>

namespace security {
  namespace {
    static_assert(EncryptedFileNonceSize == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    static_assert(EncryptedFileTagSize == crypto_aead_xchacha20poly1305_ietf_ABYTES);

    using ByteVector = std::vector<std::uint8_t>;

    [[nodiscard]] bool appendLength(ByteVector& output, std::size_t length) {
      if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (length > std::numeric_limits<std::uint64_t>::max()) {
          return false;
        }
      }
      const auto encoded = static_cast<std::uint64_t>(length);
      for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(encoded >> static_cast<unsigned>(shift)));
      }
      return true;
    }

    [[nodiscard]] bool appendField(ByteVector& output, std::string_view value) {
      if (!appendLength(output, value.size())) {
        return false;
      }
      output.insert(output.end(), value.begin(), value.end());
      return true;
    }

    [[nodiscard]] std::optional<ByteVector> associatedData(const EncryptionContext& context) {
      ByteVector output;
      output.reserve(EncryptedFileMagic.size() + 2 + 16 + context.purpose.size() + context.objectId.size());
      output.insert(output.end(), EncryptedFileMagic.begin(), EncryptedFileMagic.end());
      output.push_back(EncryptedFileFormatVersion);
      output.push_back(EncryptedFileAlgorithmXChaCha20Poly1305);
      if (!appendField(output, context.purpose) || !appendField(output, context.objectId)) {
        return std::nullopt;
      }
      return output;
    }

    [[nodiscard]] bool writePrivateEnvelope(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
      const auto target = resolveAtomicWriteTarget(path);
      if (!target.has_value() || target->path.parent_path().empty()) {
        return false;
      }

      std::error_code ec;
      if (!FileUtils::createPrivateDirectories(target->path.parent_path(), ec)) {
        return false;
      }

      const auto content = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      return writeTextFileAtomic(path, content, FileUtils::privateFileMode());
    }

    [[nodiscard]] bool readExact(std::ifstream& input, std::span<std::uint8_t> bytes) {
      if (bytes.empty()) {
        return true;
      }
      if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
      }
      input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      return input.good();
    }
  } // namespace

  bool writeEncryptedFile(
      const std::filesystem::path& path, std::span<const std::uint8_t> plaintext, const SecureKey& key,
      const EncryptionContext& context
  ) {
    if (!securityPrimitivesReady()
        || plaintext.size() > static_cast<std::size_t>(crypto_aead_xchacha20poly1305_ietf_MESSAGEBYTES_MAX)) {
      return false;
    }
    const auto aad = associatedData(context);
    if (!aad.has_value()) {
      return false;
    }

    ByteVector envelope(EncryptedFileHeaderSize + plaintext.size() + EncryptedFileTagSize);
    std::ranges::copy(EncryptedFileMagic, envelope.begin());
    envelope[EncryptedFileMagic.size()] = EncryptedFileFormatVersion;
    envelope[EncryptedFileMagic.size() + 1] = EncryptedFileAlgorithmXChaCha20Poly1305;

    auto* nonce = envelope.data() + EncryptedFileMagic.size() + 2;
    randombytes_buf(nonce, EncryptedFileNonceSize);

    unsigned long long ciphertextSize = 0;
    const int encrypted = crypto_aead_xchacha20poly1305_ietf_encrypt(
        envelope.data() + EncryptedFileHeaderSize, &ciphertextSize, plaintext.data(), plaintext.size(), aad->data(),
        aad->size(), nullptr, nonce, key.bytes().data()
    );
    if (encrypted != 0 || ciphertextSize != plaintext.size() + EncryptedFileTagSize) {
      return false;
    }
    return writePrivateEnvelope(path, envelope);
  }

  EncryptedReadResult readEncryptedFile(
      const std::filesystem::path& path, const SecureKey& key, const EncryptionContext& context,
      std::size_t maxPlaintextBytes
  ) {
    if (!securityPrimitivesReady()) {
      return {.status = EncryptedReadStatus::IoError};
    }

    std::error_code ec;
    const auto fileStatus = std::filesystem::status(path, ec);
    if (ec) {
      if (ec == std::errc::no_such_file_or_directory) {
        return {.status = EncryptedReadStatus::NotFound};
      }
      return {.status = EncryptedReadStatus::IoError};
    }
    if (!std::filesystem::is_regular_file(fileStatus)) {
      return {.status = EncryptedReadStatus::IoError};
    }

    const std::uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
      return {.status = EncryptedReadStatus::IoError};
    }
    if (fileSize < EncryptedFileHeaderSize + EncryptedFileTagSize) {
      return {.status = EncryptedReadStatus::Truncated};
    }

    const std::uintmax_t ciphertextSize = fileSize - EncryptedFileHeaderSize;
    const std::uintmax_t plaintextSize = ciphertextSize - EncryptedFileTagSize;
    if (plaintextSize > maxPlaintextBytes
        || ciphertextSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
      return {.status = EncryptedReadStatus::Oversized};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
      return {.status = EncryptedReadStatus::IoError};
    }

    std::array<std::uint8_t, EncryptedFileHeaderSize> header{};
    if (!readExact(input, header)) {
      return {.status = EncryptedReadStatus::Truncated};
    }
    if (!std::ranges::equal(EncryptedFileMagic, std::span(header).first(EncryptedFileMagic.size()))) {
      return {.status = EncryptedReadStatus::InvalidHeader};
    }
    if (header[EncryptedFileMagic.size()] != EncryptedFileFormatVersion) {
      return {.status = EncryptedReadStatus::UnsupportedVersion};
    }
    if (header[EncryptedFileMagic.size() + 1] != EncryptedFileAlgorithmXChaCha20Poly1305) {
      return {.status = EncryptedReadStatus::UnsupportedAlgorithm};
    }

    ByteVector ciphertext(static_cast<std::size_t>(ciphertextSize));
    if (!readExact(input, ciphertext)) {
      return {.status = EncryptedReadStatus::Truncated};
    }
    if (input.peek() != std::char_traits<char>::eof()) {
      return {.status = EncryptedReadStatus::IoError};
    }

    const auto aad = associatedData(context);
    if (!aad.has_value()) {
      return {.status = EncryptedReadStatus::IoError};
    }

    ByteVector plaintext(static_cast<std::size_t>(plaintextSize));
    unsigned long long decryptedSize = 0;
    const auto nonce = std::span(header).subspan(EncryptedFileMagic.size() + 2, EncryptedFileNonceSize);
    const int decrypted = crypto_aead_xchacha20poly1305_ietf_decrypt(
        plaintext.data(), &decryptedSize, nullptr, ciphertext.data(), ciphertext.size(), aad->data(), aad->size(),
        nonce.data(), key.bytes().data()
    );
    if (decrypted != 0 || decryptedSize != plaintext.size()) {
      return {.status = EncryptedReadStatus::AuthenticationFailed};
    }
    return {.status = EncryptedReadStatus::Success, .plaintext = std::move(plaintext)};
  }

} // namespace security
