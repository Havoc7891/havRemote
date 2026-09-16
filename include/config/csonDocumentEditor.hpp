// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_CONFIG_CSON_DOCUMENT_EDITOR_HPP
#define HAVREMOTE_INCLUDE_CONFIG_CSON_DOCUMENT_EDITOR_HPP

#include <havCSON.hpp>

#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

static_assert(havCSON::VersionMajor == 0 && havCSON::VersionMinor == 5,
              "havRemote requires a havCSON 0.5.x lossless API");

namespace havremote::config
{
  // Adapts havCSON's transactional editor to the repository's container handles
  // and detached, UUID-matched site nodes. The edited container stays at its
  // original address. Its children must be reacquired after a mutation.
  class CsonDocumentEditor final
  {
  public:
    static constexpr std::size_t Append = std::numeric_limits<std::size_t>::max();

    explicit CsonDocumentEditor(havCSON::LosslessValue &root) noexcept;

    [[nodiscard]] havCSON::LosslessValue &Root() noexcept;
    [[nodiscard]] const havCSON::LosslessValue &Root() const noexcept;

    [[nodiscard]] havCSON::LosslessValue *FindMember(
        havCSON::LosslessValue &object, std::string_view key) noexcept;
    [[nodiscard]] const havCSON::LosslessValue *FindMember(
        const havCSON::LosslessValue &object, std::string_view key) const noexcept;

    [[nodiscard]] bool InsertMember(havCSON::LosslessValue &object,
                                    std::string key,
                                    havCSON::LosslessValue value,
                                    std::size_t position = Append);
    [[nodiscard]] bool ReplaceMember(havCSON::LosslessValue &object,
                                     std::string_view key,
                                     havCSON::LosslessValue replacement);
    [[nodiscard]] bool RenameMember(havCSON::LosslessValue &object,
                                    std::string_view oldKey,
                                    std::string newKey);
    [[nodiscard]] bool RemoveMember(havCSON::LosslessValue &object,
                                    std::string_view key);

    [[nodiscard]] bool AppendArrayItem(havCSON::LosslessValue &array,
                                       havCSON::LosslessValue value);
    [[nodiscard]] bool ReplaceArrayItem(havCSON::LosslessValue &array,
                                        std::size_t index,
                                        havCSON::LosslessValue replacement);
    [[nodiscard]] bool RemoveArrayItem(havCSON::LosslessValue &array,
                                       std::size_t index);
    [[nodiscard]] std::optional<havCSON::LosslessValue> TakeArrayItem(
        havCSON::LosslessValue &array, std::size_t index);
    [[nodiscard]] bool SetArrayItems(havCSON::LosslessValue &array,
                                     std::vector<havCSON::LosslessValue> items);

    // Repairs semantic container values from the ordered lossless children
    void Synchronize(havCSON::LosslessValue &value);
    [[nodiscard]] bool IsSynchronized(const havCSON::LosslessValue &value) const;

  private:
    havCSON::LosslessValue *mRoot;

    [[nodiscard]] bool Edit(
        havCSON::LosslessValue &container,
        const std::function<bool(havCSON::LosslessDocumentEditor &,
                                 const havCSON::ValuePath &)> &operation,
        havCSON::LosslessEditOptions options = {});
  };
} // namespace havremote::config

#endif // HAVREMOTE_INCLUDE_CONFIG_CSON_DOCUMENT_EDITOR_HPP
