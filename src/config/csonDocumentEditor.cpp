// SPDX-License-Identifier: MIT

#include "config/csonDocumentEditor.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace havremote::config
{
  namespace
  {
    bool FindContainerPath(const havCSON::LosslessValue &current,
                           const havCSON::LosslessValue &target,
                           havCSON::ValuePath &path)
    {
      if (&current == &target)
      {
        return true;
      }

      for (const auto &[key, child] : current.objectItems)
      {
        path.emplace_back(key);

        if (FindContainerPath(child, target, path))
        {
          return true;
        }

        path.pop_back();
      }

      for (std::size_t index = 0; index < current.arrayItems.size(); ++index)
      {
        path.emplace_back(index);

        if (FindContainerPath(current.arrayItems[index], target, path))
        {
          return true;
        }

        path.pop_back();
      }

      return false;
    }

    havCSON::LosslessValue &Child(havCSON::LosslessValue &parent,
                                  const havCSON::ValuePathSegment &segment)
    {
      if (const auto *key = std::get_if<std::string>(&segment))
      {
        return std::find_if(parent.objectItems.begin(), parent.objectItems.end(),
                            [&](const auto &item)
                            { return item.first == *key; })
            ->second;
      }

      return parent.arrayItems[std::get<std::size_t>(segment)];
    }

    // Upstream has already validated, synchronized and edited this transaction.
    // Ancestors retain their ordered storage, keeping unrelated handles valid.
    void CommitContainer(havCSON::LosslessValue &original,
                         havCSON::LosslessValue &updated,
                         const havCSON::ValuePath &path, std::size_t depth = 0)
    {
      if (depth == path.size())
      {
        original = std::move(updated);

        return;
      }

      original.value = std::move(updated.value);

      CommitContainer(Child(original, path[depth]), Child(updated, path[depth]),
                      path, depth + 1);
    }

    // Copy only the semantic results of upstream rebuilding, not its ordered
    // storage. Synchronization must not invalidate repository container handles.
    void CommitSemanticValues(havCSON::LosslessValue &original,
                              havCSON::LosslessValue &rebuilt)
    {
      original.value = std::move(rebuilt.value);

      for (std::size_t index = 0; index < original.objectItems.size(); ++index)
      {
        CommitSemanticValues(original.objectItems[index].second,
                             rebuilt.objectItems[index].second);
      }

      for (std::size_t index = 0; index < original.arrayItems.size(); ++index)
      {
        CommitSemanticValues(original.arrayItems[index], rebuilt.arrayItems[index]);
      }
    }
  } // namespace

  CsonDocumentEditor::CsonDocumentEditor(havCSON::LosslessValue &root) noexcept
      : mRoot(&root) {}

  havCSON::LosslessValue &CsonDocumentEditor::Root() noexcept { return *mRoot; }
  const havCSON::LosslessValue &CsonDocumentEditor::Root() const noexcept { return *mRoot; }

  havCSON::LosslessValue *CsonDocumentEditor::FindMember(
      havCSON::LosslessValue &object, std::string_view key) noexcept
  {
    const auto found = std::find_if(object.objectItems.begin(), object.objectItems.end(),
                                    [key](const auto &item)
                                    { return item.first == key; });

    return found == object.objectItems.end() ? nullptr : &found->second;
  }

  const havCSON::LosslessValue *CsonDocumentEditor::FindMember(
      const havCSON::LosslessValue &object, std::string_view key) const noexcept
  {
    const auto found = std::find_if(object.objectItems.begin(), object.objectItems.end(),
                                    [key](const auto &item)
                                    { return item.first == key; });

    return found == object.objectItems.end() ? nullptr : &found->second;
  }

  bool CsonDocumentEditor::Edit(
      havCSON::LosslessValue &container,
      const std::function<bool(havCSON::LosslessDocumentEditor &,
                               const havCSON::ValuePath &)> &operation,
      havCSON::LosslessEditOptions options)
  {
    havCSON::ValuePath path;

    if (!FindContainerPath(*mRoot, container, path))
    {
      havCSON::LosslessDocumentEditor editor(container, options);

      return operation(editor, {});
    }

    auto transaction = *mRoot;
    havCSON::LosslessDocumentEditor editor(transaction, options);
    if (!operation(editor, path))
    {
      return false;
    }

    CommitContainer(*mRoot, transaction, path);

    return true;
  }

  bool CsonDocumentEditor::InsertMember(havCSON::LosslessValue &object,
                                        std::string key, havCSON::LosslessValue value,
                                        std::size_t position)
  {
    return Edit(object, [&](auto &editor, const auto &path)
                { return editor.InsertMember(path, std::move(key), std::move(value), position); });
  }

  bool CsonDocumentEditor::ReplaceMember(havCSON::LosslessValue &object,
                                         std::string_view key,
                                         havCSON::LosslessValue replacement)
  {
    return Edit(object, [&](auto &editor, const auto &path)
                { return editor.ReplaceMember(path, std::string(key), std::move(replacement)); });
  }

  bool CsonDocumentEditor::RenameMember(havCSON::LosslessValue &object,
                                        std::string_view oldKey, std::string newKey)
  {
    return Edit(object, [&](auto &editor, const auto &path)
                { return editor.RenameMember(path, oldKey, std::move(newKey)); });
  }

  bool CsonDocumentEditor::RemoveMember(havCSON::LosslessValue &object,
                                        std::string_view key)
  {
    return Edit(object, [&](auto &editor, const auto &path)
                { return editor.RemoveMember(path, std::string(key)); });
  }

  bool CsonDocumentEditor::AppendArrayItem(havCSON::LosslessValue &array,
                                           havCSON::LosslessValue value)
  {
    return Edit(array, [&](auto &editor, const auto &path)
                { return editor.InsertArrayItem(path, std::move(value)); });
  }

  bool CsonDocumentEditor::ReplaceArrayItem(havCSON::LosslessValue &array,
                                            std::size_t index,
                                            havCSON::LosslessValue replacement)
  {
    return Edit(array, [&](auto &editor, const auto &path)
                { return editor.ReplaceArrayItem(path, index, std::move(replacement)); });
  }

  bool CsonDocumentEditor::RemoveArrayItem(havCSON::LosslessValue &array,
                                           std::size_t index)
  {
    return Edit(array, [&](auto &editor, const auto &path)
                { return editor.RemoveArrayItem(path, index); });
  }

  bool CsonDocumentEditor::SetArrayItems(
      havCSON::LosslessValue &array, std::vector<havCSON::LosslessValue> items)
  {
    if (!array.value.isArray())
    {
      return false;
    }

    auto replacement = array;
    replacement.arrayItems = std::move(items);
    replacement.value = havCSON::Array{};

    if (!havCSON::RebuildLosslessTree(replacement))
    {
      return false;
    }

    // The repository has already matched/reordered complete nodes by stable
    // identity and removed obsolete entries through RemoveArrayItem. Replacing
    // by index would incorrectly merge another site's comments into each node.
    array = std::move(replacement);
    havCSON::ValuePath path;
    if (FindContainerPath(*mRoot, array, path))
    {
      Synchronize(*mRoot);
    }

    return true;
  }

  std::optional<havCSON::LosslessValue> CsonDocumentEditor::TakeArrayItem(
      havCSON::LosslessValue &array, std::size_t index)
  {
    if (!array.value.isArray() || index >= array.arrayItems.size())
    {
      return std::nullopt;
    }

    auto item = array.arrayItems[index];
    havCSON::LosslessEditOptions options;
    options.removedComments = havCSON::RemovedCommentPolicy::Discard;

    if (!Edit(array, [&](auto &editor, const auto &path)
              { return editor.RemoveArrayItem(path, index); }, options))
    {
      return std::nullopt;
    }

    return item;
  }

  void CsonDocumentEditor::Synchronize(havCSON::LosslessValue &value)
  {
    auto rebuilt = value;
    havCSON::Error error;
    if (!havCSON::RebuildLosslessTree(rebuilt, &error))
    {
      throw std::logic_error("Invalid configuration lossless tree: " + error.message);
    }

    CommitSemanticValues(value, rebuilt);
  }

  bool CsonDocumentEditor::IsSynchronized(const havCSON::LosslessValue &value) const
  {
    return havCSON::ValidateLosslessTree(value);
  }
} // namespace havremote::config
