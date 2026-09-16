// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_PLATFORM_CREDENTIAL_STORE_HPP
#define HAVREMOTE_INCLUDE_PLATFORM_CREDENTIAL_STORE_HPP

#include "platform/platformError.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace havremote::platform
{
  class CredentialPayload final
  {
  public:
    CredentialPayload() = default;
    CredentialPayload(std::string username, std::vector<std::byte> secret);
    ~CredentialPayload();

    CredentialPayload(const CredentialPayload &) = delete;
    CredentialPayload &operator=(const CredentialPayload &) = delete;
    CredentialPayload(CredentialPayload &&other) noexcept;
    CredentialPayload &operator=(CredentialPayload &&other) noexcept;

    [[nodiscard]] const std::string &Username() const noexcept { return mUsername; }
    [[nodiscard]] std::span<const std::byte> Secret() const noexcept { return mSecret; }

  private:
    void Clear() noexcept;

    std::string mUsername;
    std::vector<std::byte> mSecret;
  };

  class ICredentialStore
  {
  public:
    virtual ~ICredentialStore() = default;

    [[nodiscard]] virtual Result<void> Store(
        std::string_view credentialId,
        std::string_view username,
        std::span<const std::byte> secret) = 0;
    [[nodiscard]] virtual Result<CredentialPayload> Load(std::string_view credentialId) const = 0;
    [[nodiscard]] virtual Result<void> Erase(std::string_view credentialId) = 0;
  };

  // Uses the operating system's secure store. An unavailable store is reported
  // to the caller, never replaced with plaintext storage.
  [[nodiscard]] std::unique_ptr<ICredentialStore> MakeCredentialStore();
} // namespace havremote::platform

#endif // HAVREMOTE_INCLUDE_PLATFORM_CREDENTIAL_STORE_HPP
