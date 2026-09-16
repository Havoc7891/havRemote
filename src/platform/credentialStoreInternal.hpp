// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_SRC_PLATFORM_CREDENTIAL_STORE_INTERNAL_HPP
#define HAVREMOTE_SRC_PLATFORM_CREDENTIAL_STORE_INTERNAL_HPP

#include "platform/credentialStore.hpp"

#include <wx/secretstore.h>

namespace havremote::platform::detail
{
  // wxWidgets reports missing entries without logging
  class ISecretStoreBackend
  {
  public:
    virtual ~ISecretStoreBackend() = default;
    [[nodiscard]] virtual bool IsOk(wxString &reason) const = 0;
    [[nodiscard]] virtual bool Save(const wxString &service,
                                    const wxString &username,
                                    const wxSecretValue &secret) = 0;
    [[nodiscard]] virtual bool Load(const wxString &service,
                                    wxString &username,
                                    wxSecretValue &secret) const = 0;
    [[nodiscard]] virtual bool Delete(const wxString &service) = 0;
  };

  [[nodiscard]] std::unique_ptr<ICredentialStore> MakeCredentialStoreWithBackend(
      std::unique_ptr<ISecretStoreBackend> backend);
}

#endif // HAVREMOTE_SRC_PLATFORM_CREDENTIAL_STORE_INTERNAL_HPP
