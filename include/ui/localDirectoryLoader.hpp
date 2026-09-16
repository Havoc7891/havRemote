// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_LOADER_HPP
#define HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_LOADER_HPP

#include "ui/localDirectoryEvents.hpp"

#include <wx/event.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

namespace havremote::ui
{
  wxDECLARE_EVENT(EVT_HAVREMOTE_LOCAL_DIRECTORY, wxThreadEvent);

  class LocalDirectoryLoader final
  {
  public:
    // The target must outlive this loader. In a wx window, declare the loader
    // as a member so its target is invalidated before the wxEvtHandler base is
    // destroyed.
    LocalDirectoryLoader(wxEvtHandler &eventTarget, std::string connectionId);
    ~LocalDirectoryLoader();

    LocalDirectoryLoader(const LocalDirectoryLoader &) = delete;
    LocalDirectoryLoader &operator=(const LocalDirectoryLoader &) = delete;
    LocalDirectoryLoader(LocalDirectoryLoader &&) = delete;
    LocalDirectoryLoader &operator=(LocalDirectoryLoader &&) = delete;

    // Starts an asynchronous enumeration. Any prior request is superseded
    // without waiting for a potentially blocked filesystem call. Calls and
    // destruction must occur on the owning UI thread.
    [[nodiscard]] std::uint64_t Load(std::filesystem::path directory);
    void Cancel();

    [[nodiscard]] std::uint64_t CurrentGeneration() const noexcept;

  private:
    struct SharedState;
    static void Enumerate(std::shared_ptr<SharedState> state,
                          std::filesystem::path directory,
                          std::uint64_t generation,
                          std::stop_token stopToken);

    std::shared_ptr<SharedState> mState;
    std::stop_source mCurrentStopSource;
  };
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_LOCAL_DIRECTORY_LOADER_HPP
