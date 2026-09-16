// SPDX-License-Identifier: MIT

#include "config/siteTransfer.hpp"

#include <havCSON.hpp>

#include <algorithm>
#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace havremote::config
{
  namespace
  {
    constexpr std::size_t MaximumFileBytes = 4U * 1024U * 1024U;
    constexpr std::size_t MaximumSyntaxDepth = 256;
    constexpr std::size_t MaximumEntries = 10'000;
    constexpr std::size_t MaximumFolderDepth = 32;
    constexpr std::size_t MaximumTextBytes = 16U * 1024U;

    [[noreturn]] void Reject(const std::filesystem::path &file,
                             std::string message,
                             const ConfigErrorKind kind = ConfigErrorKind::Validation,
                             const havCSON::SourceInfo *source = nullptr)
    {
      throw ConfigError{kind, file, std::move(message),
                        source ? std::optional{source->valueSpan.begin.line} : std::nullopt,
                        source ? std::optional{source->valueSpan.begin.column} : std::nullopt};
    }

    template <class T>
    T Checked(std::expected<T, havCSON::AccessError> result,
              const std::filesystem::path &file)
    {
      if (!result)
      {
        const auto &error = result.error();

        Reject(file, error.message, ConfigErrorKind::Validation,
               error.source ? &*error.source : nullptr);
      }

      return std::move(*result);
    }

    const havCSON::SourceInfo *Source(const havCSON::ValueView &value)
    {
      const auto result = value.Get();
      return result ? result->get().Source() : nullptr;
    }

    std::string FileErrorMessage(const havCSON::Error &error)
    {
      auto message = error.message;

      if (!error.operation.empty())
      {
        message += " (" + error.operation + ")";
      }

      if (error.systemError)
      {
        message += ": " + error.systemError.message();
      }

      return message;
    }

    std::string PathUtf8(const std::filesystem::path &file)
    {
      const auto text = file.generic_u8string();
      return {text.begin(), text.end()};
    }

    void ValidateFilePath(const std::filesystem::path &file)
    {
      if (file.empty() ||
          file.native().find(std::filesystem::path::value_type{}) !=
              std::filesystem::path::string_type::npos)
      {
        Reject(file, "A site export filename must be nonempty and contain no NUL characters");
      }
    }

    bool ValidUtf8(const std::string_view text)
    {
      for (std::size_t i = 0; i < text.size();)
      {
        const auto first = static_cast<unsigned char>(text[i++]);

        if (first < 0x80U)
        {
          continue;
        }

        unsigned int count{};
        std::uint32_t point{};
        std::uint32_t minimum{};

        if (first >= 0xc2U && first <= 0xdfU)
        {
          count = 1;
          point = first & 0x1fU;
          minimum = 0x80U;
        }
        else if (first >= 0xe0U && first <= 0xefU)
        {
          count = 2;
          point = first & 0x0fU;
          minimum = 0x800U;
        }
        else if (first >= 0xf0U && first <= 0xf4U)
        {
          count = 3;
          point = first & 0x07U;
          minimum = 0x10000U;
        }
        else
        {
          return false;
        }

        if (count > text.size() - i)
        {
          return false;
        }

        while (count-- != 0U)
        {
          const auto next = static_cast<unsigned char>(text[i++]);

          if ((next & 0xc0U) != 0x80U)
          {
            return false;
          }

          point = (point << 6U) | (next & 0x3fU);
        }

        if (point < minimum || point > 0x10ffffU ||
            (point >= 0xd800U && point <= 0xdfffU))
        {
          return false;
        }
      }

      return true;
    }

    void ValidateText(const std::string_view text,
                      const std::string &field,
                      const std::filesystem::path &file,
                      const bool allowEmpty = false,
                      const havCSON::SourceInfo *source = nullptr)
    {
      if ((!allowEmpty && text.empty()) || text.size() > MaximumTextBytes ||
          !ValidUtf8(text) ||
          std::ranges::any_of(text, [](const unsigned char byte)
                              { return byte < 0x20U || byte == 0x7fU; }))
      {
        Reject(file, field + " must be valid UTF-8 text without control characters"
                             " (at most 16384 bytes)",
               ConfigErrorKind::Validation, source);
      }

      if (!allowEmpty && std::ranges::all_of(text, [](const char character)
                                             { return character == ' '; }))
      {
        Reject(file, field + " must not be blank", ConfigErrorKind::Validation, source);
      }
    }

    void Members(const havCSON::ValueView &value,
                 const std::initializer_list<std::string_view> keys,
                 const std::string &field,
                 const std::filesystem::path &file)
    {
      const auto object = Checked(value.AsObject(), file);

      for (const auto &[key, child] : object.get())
      {
        if (std::ranges::find(keys, key) == keys.end())
        {
          Reject(file, field + " contains unsupported field '" + key + "'",
                 ConfigErrorKind::Validation, child.Source());
        }
      }

      for (const auto key : keys)
      {
        (void)Checked(value.Member(key).Get(), file);
      }
    }

    std::string TextMember(const havCSON::ValueView &value,
                           const std::string &key,
                           const std::string &field,
                           const std::filesystem::path &file,
                           const bool allowEmpty = false)
    {
      const auto child = value.Member(key);
      const auto string = Checked(child.AsString(), file);
      const auto &text = string.get();

      ValidateText(text, field + "." + key, file, allowEmpty, Source(child));

      return text;
    }

    unsigned int IntegerMember(const havCSON::ValueView &value,
                               const std::string &key,
                               const std::filesystem::path &file,
                               const unsigned int maximum)
    {
      return Checked(value.Member(key).AsInteger<unsigned int>(1U, maximum), file);
    }

    void ValidateSite(const SiteProfile &site, const std::string &field,
                      const std::filesystem::path &file)
    {
      ValidateText(site.name, field + ".name", file);
      ValidateText(site.username, field + ".username", file, true);
      ValidateText(site.host, field + ".host", file);
      ValidateText(site.ftpEncoding, field + ".ftpEncoding", file);
      ValidateText(site.initialRemoteDirectory.Bytes(),
                   field + ".initialRemoteDirectory", file, true);

      if (!IsValidEndpointHost(site.host) || site.port == 0U)
      {
        Reject(file, field + " requires a valid host and port (1-65535)");
      }

      if (!ProtocolKindFromString(ToString(site.protocol)) ||
          !AuthenticationKindFromString(ToString(site.authentication.kind)) ||
          !FtpDataConnectionModeFromString(ToString(site.ftpDataConnectionMode)))
      {
        Reject(file, field + " contains an unsupported protocol or authentication mode");
      }

      if (site.protocol != ProtocolKind::Sftp &&
          site.authentication.kind != AuthenticationKind::Password)
      {
        Reject(file, field + ".authentication must be password for FTP/FTPS");
      }

      if (site.protocol == ProtocolKind::Sftp &&
          site.ftpDataConnectionMode != FtpDataConnectionMode::Passive)
      {
        Reject(file, field + ".ftpDataConnectionMode must be passive for SFTP");
      }
    }

    struct Import final
    {
      const std::filesystem::path &file;

      SiteTransferData data;
      std::unordered_set<std::string> generatedIds;
      std::size_t count{};

      std::string NewId()
      {
        auto id = GenerateId();

        while (!generatedIds.insert(id).second)
        {
          id = GenerateId();
        }

        return id;
      }

      void Entries(const havCSON::ValueView &value, const std::string &parent,
                   const std::string &field, const std::size_t depth)
      {
        const auto array = Checked(value.AsArray(), file);
        const auto &entries = array.get();

        std::unordered_set<std::string> names;

        for (std::size_t index = 0; index < entries.size(); ++index)
        {
          if (++count > MaximumEntries)
          {
            Reject(file, "Site export contains more than 10000 entries",
                   ConfigErrorKind::Validation, Source(value));
          }

          const auto path = field + "[" + std::to_string(index) + "]";
          const auto entry = value.At(index);

          (void)Checked(entry.AsObject(), file);

          const auto kind = TextMember(entry, "kind", path, file);
          const auto name = TextMember(entry, "name", path, file);

          if (!names.insert(name).second)
          {
            Reject(file, path + ".name duplicates another sibling entry",
                   ConfigErrorKind::Validation, Source(entry.Member("name")));
          }

          const auto id = NewId();

          data.siteManagerOrder.push_back(id);

          if (kind == "folder")
          {
            Members(entry, {"kind", "name", "entries"}, path, file);

            if (depth >= MaximumFolderDepth)
            {
              Reject(file, "Site export contains more than 32 nested folders",
                     ConfigErrorKind::Validation, Source(entry));
            }

            data.folders.push_back(SiteFolder{id, name, parent, {}});

            Entries(entry.Member("entries"), id, path + ".entries", depth + 1U);
          }
          else if (kind == "site")
          {
            Members(entry, {"kind", "name", "protocol", "host", "port", "username", "authentication", "initialRemoteDirectory", "ftpEncoding", "ftpDataConnectionMode"}, path, file);

            SiteProfile site;
            site.id = id;
            site.name = name;

            const auto protocol = ProtocolKindFromString(
                TextMember(entry, "protocol", path, file));
            const auto authentication = AuthenticationKindFromString(
                TextMember(entry, "authentication", path, file));
            const auto mode = FtpDataConnectionModeFromString(
                TextMember(entry, "ftpDataConnectionMode", path, file));

            if (!protocol)
            {
              Reject(file, path + ".protocol is unsupported", ConfigErrorKind::Validation,
                     Source(entry.Member("protocol")));
            }

            if (!authentication)
            {
              Reject(file, path + ".authentication is unsupported", ConfigErrorKind::Validation,
                     Source(entry.Member("authentication")));
            }

            if (!mode)
            {
              Reject(file, path + ".ftpDataConnectionMode is unsupported", ConfigErrorKind::Validation,
                     Source(entry.Member("ftpDataConnectionMode")));
            }

            site.protocol = *protocol;
            site.authentication.kind = *authentication;
            site.ftpDataConnectionMode = *mode;
            site.host = TextMember(entry, "host", path, file);
            site.port = static_cast<std::uint16_t>(
                IntegerMember(entry, "port", file, 65535U));
            site.username = TextMember(entry, "username", path, file, true);
            site.initialRemoteDirectory = RemotePath{
                TextMember(entry, "initialRemoteDirectory", path, file, true)};
            site.ftpEncoding = TextMember(entry, "ftpEncoding", path, file);

            try
            {
              ValidateSite(site, path, file);
            }
            catch (ConfigError &error)
            {
              // Cross-field rules describe the entry as a whole. Individual
              // type/range/text failures already refer to their exact value.
              if (const auto *source = Source(entry))
              {
                error.line = source->valueSpan.begin.line;
                error.column = source->valueSpan.begin.column;
              }

              throw;
            }

            data.sites.push_back(std::move(site));

            if (!parent.empty())
            {
              const auto folder = std::ranges::find(data.folders, parent, &SiteFolder::id);
              folder->siteIds.push_back(id);
            }
          }
          else
          {
            Reject(file, path + ".kind must be site or folder", ConfigErrorKind::Validation,
                   Source(entry.Member("kind")));
          }
        }
      }
    };

    struct Export final
    {
      const std::filesystem::path &file;
      const SiteTransferData &data;
      std::unordered_map<std::string, const SiteProfile *> sites;
      std::unordered_map<std::string, const SiteFolder *> folders;
      std::unordered_map<std::string, std::string> parents;
      std::unordered_map<std::string, std::vector<std::string>> children;
      std::size_t payloadBytes{};

      void Validate()
      {
        if (data.sites.size() + data.folders.size() > MaximumEntries)
        {
          Reject(file, "Site export contains more than 10000 entries");
        }

        for (const auto &site : data.sites)
        {
          ValidateText(site.id, "site.id", file);
          ValidateSite(site, "site", file);

          if (!sites.emplace(site.id, &site).second)
          {
            Reject(file, "Duplicate site identifier in export");
          }

          parents.emplace(site.id, "");
        }

        for (const auto &folder : data.folders)
        {
          ValidateText(folder.id, "folder.id", file);
          ValidateText(folder.name, "folder.name", file);

          if (sites.contains(folder.id) || !folders.emplace(folder.id, &folder).second)
          {
            Reject(file, "Duplicate site/folder identifier in export");
          }

          parents.emplace(folder.id, folder.parentId);
        }

        std::unordered_set<std::string> assigned;

        for (const auto &folder : data.folders)
        {
          if (!folder.parentId.empty() && !folders.contains(folder.parentId))
          {
            Reject(file, "A folder references an unknown parent");
          }

          for (const auto &siteId : folder.siteIds)
          {
            if (!sites.contains(siteId) || !assigned.insert(siteId).second)
            {
              Reject(file, "A folder references an unknown or multiply assigned site");
            }

            parents.at(siteId) = folder.id;
          }

          auto parent = folder.id;

          std::unordered_set<std::string> visited;

          while (!parent.empty())
          {
            if (!visited.insert(parent).second)
            {
              Reject(file, "Site folders contain a parent cycle");
            }

            if (visited.size() > MaximumFolderDepth)
            {
              Reject(file, "Site export contains more than 32 nested folders");
            }

            const auto found = folders.find(parent);
            if (found == folders.end())
            {
              Reject(file, "A folder references an unknown parent");
            }

            parent = found->second->parentId;
          }
        }

        std::unordered_set<std::string> ordered;

        std::unordered_map<std::string, std::unordered_set<std::string>> names;

        for (const auto &id : data.siteManagerOrder)
        {
          if (!parents.contains(id) || !ordered.insert(id).second)
          {
            Reject(file, "Site Manager order contains an unknown or duplicate identifier");
          }

          const auto &parent = parents.at(id);
          const auto &name = sites.contains(id) ? sites.at(id)->name : folders.at(id)->name;

          if (!names[parent].insert(name).second)
          {
            Reject(file, "Site Manager contains duplicate sibling names");
          }

          children[parent].push_back(id);
        }

        if (ordered.size() != parents.size())
        {
          Reject(file, "Site Manager order does not contain every site and folder");
        }
      }

      havCSON::LosslessValue Entry(const std::string &id)
      {
        auto result = havCSON::MakeLossless(havCSON::Object{});

        havCSON::LosslessDocumentEditor editor{result};

        const auto add = [&](const std::string &key, havCSON::Value value)
        {
          // Limit allocation while constructing the lossless tree, not only
          // once a potentially enormous serialized document already exists.
          payloadBytes += key.size();

          if (value.isString())
          {
            payloadBytes += std::get<std::string>(value).size();
          }

          if (payloadBytes > MaximumFileBytes)
          {
            Reject(file, "Site export exceeds the 4 MiB size limit");
          }

          havCSON::Error editError;
          if (!editor.InsertMember({}, key, havCSON::MakeLossless(value),
                                   havCSON::LosslessDocumentEditor::Append, &editError))
          {
            Reject(file, editError.message);
          }
        };

        if (const auto site = sites.find(id); site != sites.end())
        {
          const auto &s = *site->second;

          add("kind", std::string{"site"});
          add("name", s.name);
          add("protocol", std::string{ToString(s.protocol)});
          add("host", s.host);
          add("port", static_cast<double>(s.port));
          add("username", s.username);
          add("authentication", std::string{ToString(s.authentication.kind)});
          add("initialRemoteDirectory", s.initialRemoteDirectory.Bytes());
          add("ftpEncoding", s.ftpEncoding);
          add("ftpDataConnectionMode", std::string{ToString(s.ftpDataConnectionMode)});
        }
        else
        {
          add("kind", std::string{"folder"});
          add("name", folders.at(id)->name);

          havCSON::Error editError;
          if (!editor.InsertMember({}, "entries", Entries(children[id]),
                                   havCSON::LosslessDocumentEditor::Append, &editError))
          {
            Reject(file, editError.message);
          }
        }

        return result;
      }

      havCSON::LosslessValue Entries(const std::vector<std::string> &ids)
      {
        auto result = havCSON::MakeLossless(havCSON::Array{});
        result.arrayItems.reserve(ids.size());

        for (const auto &id : ids)
        {
          result.arrayItems.push_back(Entry(id));
        }

        // Build a fresh ordered array in one batch. Per-item transactional
        // edits would repeatedly copy every previously exported site.
        havCSON::Error editError;
        if (!havCSON::RebuildLosslessTree(result, &editError))
        {
          Reject(file, editError.message);
        }

        return result;
      }
    };
  } // namespace

  std::expected<SiteTransferData, ConfigError> ReadSiteExport(
      const std::filesystem::path &file)
  {
    try
    {
      ValidateFilePath(file);

      std::error_code filesystemError;
      if (!std::filesystem::is_regular_file(file, filesystemError))
      {
        Reject(file, "Site export must be a readable regular file", ConfigErrorKind::Io);
      }

      havCSON::LosslessValue parsed;
      havCSON::Error parseError;
      const havCSON::ParseOptions parseOptions{
          .trackSourceLocations = true,
          .maxDepth = MaximumSyntaxDepth,
          .maxInputBytes = MaximumFileBytes};

      const auto code = havCSON::ParseFileLossless(PathUtf8(file), parsed, &parseError,
                                                   parseOptions);

      if (code == havCSON::ErrorCode::IoError)
      {
        Reject(file, FileErrorMessage(parseError), ConfigErrorKind::Io);
      }

      if (code != havCSON::ErrorCode::OK)
      {
        throw ConfigError{code == havCSON::ErrorCode::ResourceLimit
                              ? ConfigErrorKind::Validation
                              : ConfigErrorKind::Parse,
                          file, parseError.message, parseError.where.line,
                          parseError.where.column};
      }

      const havCSON::ValueView root{parsed.value};

      (void)Checked(root.AsObject(), file);

      const auto version = IntegerMember(root, "formatVersion", file, 0xffffffffU);

      if (version != 1U)
      {
        Reject(file, "Site export format version " + std::to_string(version) + " is not supported. Expected version 1",
               ConfigErrorKind::UnsupportedVersion, Source(root.Member("formatVersion")));
      }

      if (TextMember(root, "kind", "root", file) != "havRemoteSites")
      {
        Reject(file, "This file is not a havRemote site export",
               ConfigErrorKind::UnsupportedVersion, Source(root.Member("kind")));
      }

      Members(root, {"formatVersion", "kind", "entries"}, "root", file);

      Import importer{file, {}, {}, 0};
      importer.Entries(root.Member("entries"), "", "entries", 0);

      return std::move(importer.data);
    }
    catch (const ConfigError &error)
    {
      return std::unexpected(error);
    }
    catch (const std::filesystem::filesystem_error &error)
    {
      return std::unexpected(ConfigError{ConfigErrorKind::Io, file, error.what(),
                                         std::nullopt, std::nullopt});
    }
  }

  std::expected<void, ConfigError> WriteSiteExport(
      const std::filesystem::path &file, const SiteTransferData &data,
      const std::optional<std::string> selectedRootId)
  {
    try
    {
      ValidateFilePath(file);

      Export exporter{file, data, {}, {}, {}, {}, 0};
      exporter.Validate();

      if (selectedRootId && !exporter.parents.contains(*selectedRootId))
      {
        Reject(file, "The selected export entry does not exist");
      }

      auto document = havCSON::MakeLossless(havCSON::Object{});
      havCSON::LosslessDocumentEditor editor{document};
      havCSON::Error editError;
      const auto add = [&](std::string key, havCSON::LosslessValue value)
      {
        if (!editor.InsertMember({}, std::move(key), std::move(value),
                                 havCSON::LosslessDocumentEditor::Append, &editError))
        {
          Reject(file, editError.message);
        }
      };

      add("formatVersion", havCSON::MakeLossless(1.0));
      add("kind", havCSON::MakeLossless(std::string{"havRemoteSites"}));
      add("entries", exporter.Entries(
                         selectedRootId ? std::vector<std::string>{*selectedRootId} : exporter.children[""]));

      havCSON::WriteOptions options;
      options.indentWidth = 2;
      options.sortObjectKeys = false;

      if (havCSON::ToStringLossless(document, options).size() > MaximumFileBytes)
      {
        Reject(file, "Site export exceeds the 4 MiB size limit");
      }

      havCSON::Error writeError;
      if (!havCSON::WriteFileLosslessAtomic(PathUtf8(file), document, options, &writeError))
      {
        Reject(file, writeError.message.empty() ? "Could not atomically write site export" : FileErrorMessage(writeError),
               writeError.code == havCSON::ErrorCode::IoError
                   ? ConfigErrorKind::AtomicWrite
                   : ConfigErrorKind::Validation);
      }

      return {};
    }
    catch (const ConfigError &error)
    {
      return std::unexpected(error);
    }
    catch (const std::filesystem::filesystem_error &error)
    {
      return std::unexpected(ConfigError{ConfigErrorKind::Io, file, error.what(),
                                         std::nullopt, std::nullopt});
    }
  }
} // namespace havremote::config
