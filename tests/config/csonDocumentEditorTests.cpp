// SPDX-License-Identifier: MIT

#include "config/csonDocumentEditor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

namespace havremote::config
{
  namespace
  {
    havCSON::LosslessValue Parse(std::string_view text)
    {
      havCSON::LosslessValue value;
      havCSON::Error error;

      REQUIRE(havCSON::ParseLossless(text, value, &error) == havCSON::ErrorCode::OK);

      return value;
    }

    std::string Write(const havCSON::LosslessValue &value)
    {
      havCSON::WriteOptions options;
      options.indentWidth = 2;
      options.sortObjectKeys = false;

      std::string text;

      havCSON::Error error;

      REQUIRE(havCSON::ToStringLossless(value, text, options, &error));

      return text;
    }

    havCSON::LosslessValue Number(double value)
    {
      havCSON::LosslessValue result;
      result.value = value;

      return result;
    }
  } // namespace

  TEST_CASE("CsonDocumentEditor keeps semantic and lossless object views synchronized")
  {
    auto document = Parse(R"(# document
alpha: 1 # alpha inline
beta: 2
)");

    CsonDocumentEditor editor(document);

    REQUIRE(editor.IsSynchronized(document));
    REQUIRE(editor.InsertMember(document, "gamma", Number(3)));
    REQUIRE(editor.ReplaceMember(document, "alpha", Number(10)));
    REQUIRE(editor.RenameMember(document, "beta", "renamed"));
    REQUIRE_FALSE(editor.RenameMember(document, "gamma", "renamed"));
    REQUIRE(editor.IsSynchronized(document));

    REQUIRE(document.value.asObject().contains("alpha"));
    REQUIRE(document.value.asObject().contains("renamed"));
    REQUIRE(document.value.asObject().contains("gamma"));
    REQUIRE(std::get<double>(document.value.asObject().at("alpha")) == 10);

    const std::string serialized = Write(document);

    REQUIRE(serialized.find("alpha: 10.0 # alpha inline") != std::string::npos);
    REQUIRE(serialized.find("renamed: 2.0") != std::string::npos);
    REQUIRE(serialized.find("gamma: 3.0") != std::string::npos);
  }

  TEST_CASE("Renaming retains the complete lossless node and every comment channel")
  {
    auto document = Parse(R"(oldName:
  nested: 1
)");

    CsonDocumentEditor editor(document);

    auto *original = editor.FindMember(document, "oldName");

    REQUIRE(original != nullptr);

    original->leadingComments.push_back({.indent = 0, .text = "# rename leading"});
    original->blockComment = " rename block";
    original->inlineComment = " rename inline";
    original->closingComments.push_back({.indent = 2, .text = "# rename closing"});
    original->trailingComments.push_back({.indent = 0, .text = "# rename trailing"});

    REQUIRE(editor.RenameMember(document, "oldName", "newName"));
    REQUIRE(editor.IsSynchronized(document));
    REQUIRE(editor.FindMember(document, "oldName") == nullptr);

    const auto *renamed = editor.FindMember(document, "newName");

    REQUIRE(renamed != nullptr);
    REQUIRE(renamed->leadingComments.front().text == "# rename leading");
    REQUIRE(renamed->blockComment == " rename block");
    REQUIRE(renamed->inlineComment == " rename inline");
    REQUIRE(renamed->closingComments.front().text == "# rename closing");
    REQUIRE(renamed->trailingComments.front().text == "# rename trailing");

    const std::string serialized = Write(document);

    REQUIRE(serialized.find("# rename leading") != std::string::npos);
    REQUIRE(serialized.find("newName: # rename block") != std::string::npos);
    REQUIRE(serialized.find("# rename closing") != std::string::npos);
    REQUIRE(serialized.find("} # rename inline") != std::string::npos);
    REQUIRE(serialized.find("# rename trailing") != std::string::npos);
  }

  TEST_CASE("Removing a member transfers comments to the following member")
  {
    auto document = Parse(R"(# before removed
removed: true # removed inline

# before kept
kept: 7
)");

    CsonDocumentEditor editor(document);

    REQUIRE(editor.RemoveMember(document, "removed"));
    REQUIRE(editor.IsSynchronized(document));

    const auto *kept = editor.FindMember(document, "kept");

    REQUIRE(kept != nullptr);
    REQUIRE(kept->leadingComments.size() == 3);
    REQUIRE(kept->leadingComments[0].text == "# removed inline");
    REQUIRE(kept->leadingComments[1].text.empty());
    REQUIRE(kept->leadingComments[2].text == "# before kept");

    const std::string serialized = Write(document);

    REQUIRE(serialized.find("# before removed\n# removed inline\n\n# before kept\nkept: 7.0") !=
            std::string::npos);
  }

  TEST_CASE("Removing a nested array-object member emits reparsable comments at member indentation")
  {
    auto document = Parse(R"(workspace:
  records: [
    {
      first: 1 # first inline
      removed: true # removed inline
      kept: 7
    }
  ]
)");

    CsonDocumentEditor editor(document);

    auto *workspace = editor.FindMember(document, "workspace");

    REQUIRE(workspace != nullptr);

    auto *records = editor.FindMember(*workspace, "records");

    REQUIRE(records != nullptr);
    REQUIRE(records->arrayItems.size() == 1);

    auto &record = records->arrayItems.front();

    REQUIRE(editor.RemoveMember(record, "removed"));
    REQUIRE(editor.IsSynchronized(document));

    const auto *kept = editor.FindMember(record, "kept");

    REQUIRE(kept != nullptr);
    REQUIRE(kept->leadingComments.size() == 1);
    REQUIRE(kept->leadingComments[0].indent == 8);
    REQUIRE(kept->leadingComments[0].text == "# removed inline");

    const std::string serialized = Write(document);

    REQUIRE(serialized.find("        first: 1.0 # first inline\n"
                            "        # removed inline\n"
                            "        kept: 7.0") != std::string::npos);

    auto reparsed = Parse(serialized);

    CsonDocumentEditor reparsedEditor(reparsed);

    REQUIRE(reparsedEditor.IsSynchronized(reparsed));
  }

  TEST_CASE("Removing the last member moves its comments to its container")
  {
    auto document = Parse(R"(kept: 1
# last leading
last: 2 # last inline
)");

    CsonDocumentEditor editor(document);

    REQUIRE(editor.RemoveMember(document, "last"));
    REQUIRE(editor.IsSynchronized(document));
    REQUIRE(document.trailingComments.size() == 2);
    REQUIRE(document.trailingComments[0].text == "# last leading");
    REQUIRE(document.trailingComments[1].text == "# last inline");

    const std::string serialized = Write(document);

    REQUIRE(serialized.find("kept: 1.0\n# last leading\n# last inline") !=
            std::string::npos);
  }

  TEST_CASE("Removing a container preserves comments nested inside it")
  {
    auto document = Parse(R"(obsolete:
  # nested documentation
  nested: 1 # nested inline
kept: true
)");

    CsonDocumentEditor editor(document);

    REQUIRE(editor.RemoveMember(document, "obsolete"));
    REQUIRE(editor.IsSynchronized(document));

    const std::string serialized = Write(document);

    REQUIRE(serialized.find("# nested documentation") != std::string::npos);
    REQUIRE(serialized.find("# nested inline") != std::string::npos);
    REQUIRE(serialized.find("kept: true") != std::string::npos);
  }

  TEST_CASE("Lossless edits retain block closing and trailing comments")
  {
    auto document = Parse(R"(container: # block note
  values: [
    1
    # before closing
  ]
# before next

next: 2
# document trailing
)");

    CsonDocumentEditor editor(document);

    auto *container = editor.FindMember(document, "container");

    REQUIRE(container != nullptr);

    auto *values = editor.FindMember(*container, "values");

    REQUIRE(values != nullptr);

    auto *next = editor.FindMember(document, "next");

    REQUIRE(next != nullptr);

    REQUIRE(container->blockComment == " block note");
    REQUIRE(values->closingComments.size() == 1);
    REQUIRE(values->closingComments.front().text == "# before closing");
    REQUIRE_FALSE(next->leadingComments.empty());
    REQUIRE(document.trailingComments.size() == 1);

    REQUIRE(editor.ReplaceArrayItem(*values, 0, Number(9)));
    REQUIRE(editor.IsSynchronized(document));

    const std::string serialized = Write(document);

    INFO(serialized);
    REQUIRE(serialized.find("container: # block note") != std::string::npos);
    REQUIRE(serialized.find("# before closing") != std::string::npos);

    const auto comment = serialized.find("# before next");

    const auto nextMember = serialized.find("next: 2.0");

    REQUIRE(comment != std::string::npos);
    REQUIRE(nextMember != std::string::npos);
    REQUIRE(comment < nextMember);
    // The original blank line survives. The writer may normalize its indentation.
    REQUIRE(std::count(serialized.begin() + static_cast<std::ptrdiff_t>(comment),
                       serialized.begin() + static_cast<std::ptrdiff_t>(nextMember),
                       '\n') == 2);
    REQUIRE(serialized.find("# document trailing") != std::string::npos);

    auto reparsed = Parse(serialized);

    CsonDocumentEditor reparsedEditor(reparsed);

    REQUIRE(reparsedEditor.IsSynchronized(reparsed));
  }

  TEST_CASE("Array edits keep ordered and semantic items synchronized")
  {
    auto document = Parse(R"(items: [
  1 # first
  # before second
  2
]
)");

    CsonDocumentEditor editor(document);

    auto *items = editor.FindMember(document, "items");

    REQUIRE(items != nullptr);

    REQUIRE(editor.RemoveArrayItem(*items, 0));
    REQUIRE(editor.AppendArrayItem(*items, Number(3)));
    REQUIRE(editor.ReplaceArrayItem(*items, 1, Number(4)));
    REQUIRE(editor.IsSynchronized(document));
    REQUIRE(items->arrayItems.front().leadingComments.size() == 2);
    REQUIRE(items->arrayItems.front().leadingComments[0].text == "# first");
    REQUIRE(items->arrayItems.front().leadingComments[1].text == "# before second");
    REQUIRE(std::get<double>(items->value.asArray()[1]) == 4);
  }

  TEST_CASE("Upstream edits preserve container and unrelated sibling handles")
  {
    auto document = Parse("settings:\n  local:\n    width: 100\n  remote:\n    width: 200\n");

    CsonDocumentEditor editor(document);

    auto *settings = editor.FindMember(document, "settings");

    REQUIRE(settings != nullptr);

    auto *local = editor.FindMember(*settings, "local");

    auto *remote = editor.FindMember(*settings, "remote");

    REQUIRE(local != nullptr);
    REQUIRE(remote != nullptr);

    REQUIRE(editor.ReplaceMember(*local, "width", Number(300)));
    REQUIRE(editor.InsertMember(*local, "height", Number(400)));
    REQUIRE(editor.FindMember(document, "settings") == settings);
    REQUIRE(editor.FindMember(*settings, "local") == local);
    REQUIRE(editor.FindMember(*settings, "remote") == remote);
    REQUIRE(editor.ReplaceMember(*remote, "width", Number(500)));
    REQUIRE(editor.IsSynchronized(document));

    const auto original = Write(document);

    REQUIRE_FALSE(editor.InsertMember(*local, "width", Number(600)));
    REQUIRE(Write(document) == original);
    REQUIRE(editor.FindMember(*settings, "remote") == remote);
  }

  TEST_CASE("Identity-matched array ordering does not merge comments by index")
  {
    auto document = Parse("items: [\n  1 # first item\n  2 # second item\n]\n");

    for (int cycle = 0; cycle < 5; ++cycle)
    {
      CsonDocumentEditor editor(document);

      auto *items = editor.FindMember(document, "items");

      REQUIRE(items != nullptr);

      auto reversed = items->arrayItems;

      std::reverse(reversed.begin(), reversed.end());

      REQUIRE(editor.SetArrayItems(*items, std::move(reversed)));
      REQUIRE(editor.IsSynchronized(document));

      const auto written = Write(document);

      REQUIRE(written.find("1.0 # first item") != std::string::npos);
      REQUIRE(written.find("2.0 # second item") != std::string::npos);
      REQUIRE(written.find("first item") == written.rfind("first item"));
      REQUIRE(written.find("second item") == written.rfind("second item"));

      document = Parse(written);
    }
  }

  TEST_CASE("Taking an array item moves comments instead of leaving copies")
  {
    auto document = Parse("items: [\n  1 # move with item\n  2\n]\n");

    CsonDocumentEditor editor(document);

    auto *items = editor.FindMember(document, "items");

    REQUIRE(items != nullptr);

    auto detached = editor.TakeArrayItem(*items, 0);

    REQUIRE(detached.has_value());
    REQUIRE(editor.IsSynchronized(document));
    REQUIRE(Write(document).find("move with item") == std::string::npos);
    REQUIRE(editor.AppendArrayItem(*items, std::move(*detached)));

    const auto written = Write(document);

    REQUIRE(written.find("1.0 # move with item") != std::string::npos);
    REQUIRE(written.find("move with item") == written.rfind("move with item"));
  }
} // namespace havremote::config
