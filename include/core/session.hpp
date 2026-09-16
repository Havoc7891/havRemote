// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CORE_SESSION_HPP
#define HAVREMOTE_INCLUDE_CORE_SESSION_HPP

#include "core/types.hpp"

#include <memory>
#include <stop_token>
#include <vector>

namespace havremote
{
  // A session is owned and called by one worker. Implementations are not
  // required to permit concurrent member calls.
  class IRemoteSession
  {
  public:
    virtual ~IRemoteSession() = default;

    [[nodiscard]] virtual ProtocolKind Protocol() const noexcept = 0;
    [[nodiscard]] virtual bool Connected() const noexcept = 0;

    virtual Result<void> Connect(const SiteProfile &site,
                                 const SessionCallbacks &callbacks,
                                 std::stop_token stopToken) = 0;
    virtual void Disconnect() noexcept = 0;

    virtual Result<std::vector<RemoteEntry>> List(const RemotePath &path,
                                                  std::stop_token stopToken) = 0;
    virtual Result<RemoteEntry> Stat(const RemotePath &path,
                                     std::stop_token stopToken) = 0;
    virtual Result<void> Mkdir(const RemotePath &path,
                               bool recursive,
                               std::stop_token stopToken) = 0;
    virtual Result<void> CreateRemoteFile(const RemotePath &path,
                                          bool overwrite,
                                          std::stop_token stopToken) = 0;
    virtual Result<void> Rename(const RemotePath &source,
                                const RemotePath &destination,
                                bool overwrite,
                                std::stop_token stopToken) = 0;
    virtual Result<void> Remove(const RemotePath &path,
                                bool recursive,
                                std::stop_token stopToken) = 0;
    virtual Result<void> SetPermissions(const RemotePath &path,
                                        std::uint32_t permissions,
                                        std::stop_token stopToken) = 0;

    virtual Result<void> Upload(const std::filesystem::path &localPath,
                                const RemotePath &remotePath,
                                const TransferOptions &options,
                                const ProgressCallback &progress,
                                std::stop_token stopToken) = 0;
    virtual Result<void> Download(const RemotePath &remotePath,
                                  const std::filesystem::path &localPath,
                                  const TransferOptions &options,
                                  const ProgressCallback &progress,
                                  std::stop_token stopToken) = 0;
  };

  using RemoteSessionPtr = std::unique_ptr<IRemoteSession>;
} // namespace havremote

#endif // HAVREMOTE_INCLUDE_CORE_SESSION_HPP
