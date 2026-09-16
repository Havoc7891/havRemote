// SPDX-License-Identifier: MIT

#include "platform/credentialStore.hpp"
#include "credentialStoreInternal.hpp"

#include <wx/log.h>
#include <wx/thread.h>
#include <wx/utils.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace havremote::platform
{
  namespace
  {
    class CredentialDiagnostics final
    {
    public:
      CredentialDiagnostics()
          : mLoggingWasEnabled(wxLog::EnableLogging(true))
      {
#if wxUSE_THREADS
        if (!wxIsMainThread())
        {
          delete mThreadLog.SetFormatter(new wxLogFormatterNone{});

          mPreviousThreadLog = wxLog::SetThreadActiveTarget(&mThreadLog);

          mUsesThreadLog = true;

          return;
        }
#endif

        mMainLog.emplace();
      }

      ~CredentialDiagnostics()
      {
#if wxUSE_THREADS
        if (mUsesThreadLog)
        {
          mThreadLog.Clear();
          wxLog::SetThreadActiveTarget(mPreviousThreadLog);
        }
#endif

        mMainLog.reset();

        wxLog::EnableLogging(mLoggingWasEnabled);
      }

      [[nodiscard]] const wxString &GetMessages() const
      {
        return mMainLog ? mMainLog->GetMessages() : mThreadLog.GetBuffer();
      }

    private:
      // wxLogCollector handles the GUI thread. Worker errors need a
      // thread-local target so they aren't queued for a second GUI dialog.
      std::optional<wxLogCollector> mMainLog;
      wxLogBuffer mThreadLog;
      wxLog *mPreviousThreadLog{};
      bool mUsesThreadLog{};
      bool mLoggingWasEnabled{};
    };

    Result<wxString> CredentialText(const std::string_view value,
                                    const std::string_view field)
    {
      if (value.empty() || value.find_first_of(std::string_view{"\0\r\n", 3}) !=
                               std::string_view::npos)
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::InvalidArgument,
            std::string{field} + " must not be empty or contain NUL or line breaks"});
      }

      const auto decoded = wxString::FromUTF8(value.data(), value.size());

      bool valid = !decoded.empty();

#if !wxUSE_UNICODE_UTF16
      // Reject non-scalar values before wxWidgets re-encodes them to UTF-8
      for (const auto character : decoded)
      {
        const auto scalar = character.GetValue();

        if ((scalar >= 0xd800U && scalar <= 0xdfffU) || scalar > 0x10ffffU)
        {
          valid = false;
        }
      }
#endif
      if (!valid || decoded.ToStdString(wxConvUTF8) != value)
      {
        return std::unexpected(PlatformError{
            PlatformErrorCode::InvalidArgument,
            std::string{field} + " is not valid UTF-8"});
      }

      return decoded;
    }

    Result<wxString> ServiceName(const std::string_view credentialId)
    {
      auto identifier = CredentialText(credentialId, "Credential identifier");
      if (!identifier)
      {
        return std::unexpected(identifier.error());
      }

      return "havRemote/" + *identifier;
    }

    PlatformError StoreError(const PlatformErrorCode code,
                             std::string message,
                             const wxString &detail = {})
    {
      if (!detail.empty())
      {
        message += ": ";
        message += detail.Left(2048).ToStdString(wxConvUTF8);

        while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
        {
          message.pop_back();
        }
      }

      return PlatformError{code, std::move(message)};
    }

    class SystemSecretStoreBackend final : public detail::ISecretStoreBackend
    {
    public:
      bool IsOk(wxString &reason) const override
      {
#if wxUSE_SECRETSTORE
        // Open lazily so ordinary application startup does not prompt to unlock
        // a keyring. Retry an unavailable service when credentials are next used.
        if (!mStore)
        {
          mStore.emplace(wxSecretStore::GetDefault());
        }

        if (mStore->IsOk(&reason))
        {
          return true;
        }

        mStore.reset();

        return false;
#else
        reason = "Secure credential storage is not included in this build";

        return false;
#endif
      }

      bool Save(const wxString &service,
                const wxString &username,
                const wxSecretValue &secret) override
      {
#if wxUSE_SECRETSTORE
        return mStore && mStore->Save(service, username, secret);
#else
        (void)service;
        (void)username;
        (void)secret;

        return false;
#endif
      }

      bool Load(const wxString &service,
                wxString &username,
                wxSecretValue &secret) const override
      {
#if wxUSE_SECRETSTORE
        return mStore && mStore->Load(service, username, secret);
#else
        (void)service;
        (void)username;
        (void)secret;

        return false;
#endif
      }

      bool Delete(const wxString &service) override
      {
#if wxUSE_SECRETSTORE
        return mStore && mStore->Delete(service);
#else
        (void)service;

        return false;
#endif
      }

    private:
#if wxUSE_SECRETSTORE
      mutable std::optional<wxSecretStore> mStore;
#endif
    };

    class CredentialStore final : public ICredentialStore
    {
    public:
      explicit CredentialStore(std::unique_ptr<detail::ISecretStoreBackend> backend)
          : mBackend(std::move(backend)) {}

      Result<void> Store(const std::string_view credentialId,
                         const std::string_view username,
                         const std::span<const std::byte> secret) override
      {
        const std::lock_guard lock{mMutex};

        auto service = ServiceName(credentialId);
        if (!service)
        {
          return std::unexpected(service.error());
        }

        auto user = CredentialText(username, "Credential username");
        if (!user)
        {
          return std::unexpected(user.error());
        }

        if (secret.size() > (std::numeric_limits<std::uint32_t>::max)())
        {
          return std::unexpected(StoreError(PlatformErrorCode::InvalidArgument,
                                             "Credential secret is too large"));
        }

        CredentialDiagnostics errors;

        if (const auto ready = RequireAvailable(); !ready)
        {
          return ready;
        }

        const std::byte empty{};
        const wxSecretValue value{secret.size(), secret.empty() ? &empty : secret.data()};

        if (!mBackend->Save(*service, *user, value))
        {
          return std::unexpected(StoreError(PlatformErrorCode::OperatingSystem,
                                             "Could not save credential",
                                             errors.GetMessages()));
        }

        return {};
      }

      Result<CredentialPayload> Load(const std::string_view credentialId) const override
      {
        const std::lock_guard lock{mMutex};

        auto service = ServiceName(credentialId);
        if (!service)
        {
          return std::unexpected(service.error());
        }

        CredentialDiagnostics errors;

        if (const auto ready = RequireAvailable(); !ready)
        {
          return std::unexpected(ready.error());
        }

        wxString username;
        wxSecretValue secret;

        if (!mBackend->Load(*service, username, secret))
        {
          if (errors.GetMessages().empty())
          {
            return std::unexpected(StoreError(PlatformErrorCode::NotFound,
                                               "Credential was not found"));
          }

          return std::unexpected(StoreError(PlatformErrorCode::OperatingSystem,
                                             "Could not read credential",
                                             errors.GetMessages()));
        }

        if (!secret.IsOk() || (secret.GetSize() != 0U && !secret.GetData()))
        {
          return std::unexpected(StoreError(PlatformErrorCode::OperatingSystem,
                                             "The secure store returned an invalid credential"));
        }

        std::vector<std::byte> bytes(secret.GetSize());
        if (!bytes.empty())
        {
          std::copy_n(static_cast<const std::byte *>(secret.GetData()),
                      bytes.size(), bytes.begin());
        }

        return CredentialPayload{username.ToStdString(wxConvUTF8), std::move(bytes)};
      }

      Result<void> Erase(const std::string_view credentialId) override
      {
        const std::lock_guard lock{mMutex};

        auto service = ServiceName(credentialId);
        if (!service)
        {
          return std::unexpected(service.error());
        }

        CredentialDiagnostics errors;

        if (const auto ready = RequireAvailable(); !ready)
        {
          return ready;
        }

        if (!mBackend->Delete(*service) && !errors.GetMessages().empty())
        {
          return std::unexpected(StoreError(PlatformErrorCode::OperatingSystem,
                                             "Could not delete credential",
                                             errors.GetMessages()));
        }

        return {};
      }

    private:
      Result<void> RequireAvailable() const
      {
        wxString reason;

        if (!mBackend || !mBackend->IsOk(reason))
        {
          return std::unexpected(StoreError(PlatformErrorCode::Unavailable,
                                             "The operating system's secure credential store is unavailable",
                                             reason));
        }

        return {};
      }

      std::unique_ptr<detail::ISecretStoreBackend> mBackend;
      mutable std::mutex mMutex;
    };
  }

  CredentialPayload::CredentialPayload(std::string username, std::vector<std::byte> secret)
      : mUsername(std::move(username)), mSecret(std::move(secret)) {}

  CredentialPayload::~CredentialPayload() { Clear(); }

  CredentialPayload::CredentialPayload(CredentialPayload &&other) noexcept
      : mUsername(std::move(other.mUsername)), mSecret(std::move(other.mSecret))
  {
    other.Clear();
  }

  CredentialPayload &CredentialPayload::operator=(CredentialPayload &&other) noexcept
  {
    if (this != &other)
    {
      Clear();

      mUsername = std::move(other.mUsername);
      mSecret = std::move(other.mSecret);

      other.Clear();
    }

    return *this;
  }

  void CredentialPayload::Clear() noexcept
  {
    if (!mSecret.empty())
    {
      wxSecureZeroMemory(mSecret.data(), mSecret.size());

      mSecret.clear();
    }

    if (!mUsername.empty())
    {
      wxSecureZeroMemory(mUsername.data(), mUsername.size());

      mUsername.clear();
    }
  }

  std::unique_ptr<ICredentialStore> MakeCredentialStore()
  {
    return detail::MakeCredentialStoreWithBackend(
        std::make_unique<SystemSecretStoreBackend>());
  }

  std::unique_ptr<ICredentialStore> detail::MakeCredentialStoreWithBackend(
      std::unique_ptr<ISecretStoreBackend> backend)
  {
    return std::make_unique<CredentialStore>(std::move(backend));
  }
}
