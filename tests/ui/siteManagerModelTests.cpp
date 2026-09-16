// SPDX-License-Identifier: MIT

#include "ui/siteManagerModel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace havremote;
using namespace havremote::ui;

namespace
{
  SiteProfile Site(std::string id, std::string name)
  {
    SiteProfile result;
    result.id = std::move(id);
    result.name = std::move(name);

    return result;
  }

  std::vector<std::string> ChildIds(
      const std::vector<SiteProfile> &sites,
      const std::vector<config::SiteFolder> &folders,
      const std::vector<std::string> &order,
      const std::string_view parentId)
  {
    std::vector<std::string> result;

    for (const auto &entry :
         OrderedSiteManagerChildren(sites, folders, order, parentId))
    {
      result.push_back(entry.id);
    }

    return result;
  }

  struct OrderedTreeFixture final
  {
    std::vector<SiteProfile> sites{
        Site("root-one", "Root one"),
        Site("root-two", "Root two"),
        Site("a-one", "A one"),
        Site("a-two", "A two"),
        Site("deep", "Deep"),
        Site("b-one", "B one"),
    };

    std::vector<config::SiteFolder> folders{
        {.id = "a", .name = "A", .parentId = {}, .siteIds = {"a-one", "a-two"}},
        {.id = "a-child", .name = "A child", .parentId = "a", .siteIds = {"deep"}},
        {.id = "b", .name = "B", .parentId = {}, .siteIds = {"b-one"}},
    };

    std::vector<std::string> order{
        "a",
        "root-one",
        "b",
        "root-two",
        "a-one",
        "a-child",
        "a-two",
        "deep",
        "b-one",
    };
  };
} // namespace

TEST_CASE("changed saved secrets always receive fresh separate store identifiers",
          "[ui][sites][credentials]")
{
  Authentication original;
  original.credentialId = "existing-password";
  original.passphraseCredentialId = "existing-passphrase";

  const auto first = PrepareCredentialIdentifiers(original, true, true);
  const auto second = PrepareCredentialIdentifiers(first, true, true);

  const std::unordered_set identifiers{
      original.credentialId, original.passphraseCredentialId,
      first.credentialId, first.passphraseCredentialId,
      second.credentialId, second.passphraseCredentialId};

  CHECK(identifiers.size() == 6);
  CHECK_FALSE(identifiers.contains(std::string{}));
  CHECK(original.credentialId == "existing-password");
  CHECK(original.passphraseCredentialId == "existing-passphrase");
}

TEST_CASE("credential preparation only replaces fields containing a new secret",
          "[ui][sites][credentials]")
{
  Authentication original;
  original.kind = AuthenticationKind::PrivateKey;
  original.credentialId = "existing-password";
  original.passphraseCredentialId = "existing-passphrase";
  original.privateKeyFile = "private.key";
  original.publicKeyFile = "public.key";

  const auto passwordOnly = PrepareCredentialIdentifiers(original, true, false);

  CHECK(passwordOnly.credentialId != original.credentialId);
  CHECK(passwordOnly.passphraseCredentialId == original.passphraseCredentialId);
  CHECK(passwordOnly.kind == original.kind);
  CHECK(passwordOnly.privateKeyFile == original.privateKeyFile);
  CHECK(passwordOnly.publicKeyFile == original.publicKeyFile);

  const auto passphraseOnly = PrepareCredentialIdentifiers(original, false, true);

  CHECK(passphraseOnly.credentialId == original.credentialId);
  CHECK(passphraseOnly.passphraseCredentialId != original.passphraseCredentialId);
}

TEST_CASE("username-only edits retain their saved credential identifiers",
          "[ui][sites][credentials]")
{
  auto site = Site("saved-site", "Saved site");
  site.authentication.credentialId = "existing-password";
  site.authentication.passphraseCredentialId = "existing-passphrase";
  site.username = "renamed-user";
  site.authentication = PrepareCredentialIdentifiers(site.authentication, false, false);

  CHECK(site.authentication.credentialId == "existing-password");
  CHECK(site.authentication.passphraseCredentialId == "existing-passphrase");
}

TEST_CASE("discarding a pending credential update preserves the committed identifiers",
          "[ui][sites][credentials]")
{
  Authentication committed;
  committed.credentialId = "existing-password";
  committed.passphraseCredentialId = "existing-passphrase";
  {
    const auto candidate = PrepareCredentialIdentifiers(committed, true, true);

    CHECK(candidate.credentialId != committed.credentialId);
    CHECK(candidate.passphraseCredentialId != committed.passphraseCredentialId);
  }

  CHECK(committed.credentialId == "existing-password");
  CHECK(committed.passphraseCredentialId == "existing-passphrase");
}

TEST_CASE("site names must not duplicate another site in the same parent")
{
  const std::vector sites{
      Site("first", "Production"),
      Site("second", "Entwicklung"),
  };

  CHECK_FALSE(IsSiteNameUniqueInTree(sites, {}, "Production", {}, "second"));
  CHECK(IsSiteNameUniqueInTree(sites, {}, "Entwicklung", {}, "second"));
  CHECK(IsSiteNameUniqueInTree(sites, {}, "Staging", {}, "second"));
  CHECK(IsSiteNameUniqueInTree(sites, {}, "production", {}, "second"));
}

TEST_CASE("site name matching preserves exact Unicode labels")
{
  const std::vector sites{
      Site("first", "Büro München"),
  };

  CHECK_FALSE(IsSiteNameUniqueInTree(sites, {}, "Büro München", {}));
  CHECK(IsSiteNameUniqueInTree(sites, {}, "Büro Munchen", {}));
}

TEST_CASE("duplicate site-name detection identifies the later entry")
{
  auto sites = std::vector{
      Site("first", "Production"),
      Site("second", "Staging"),
      Site("third", "Production"),
  };

  CHECK(DuplicateSiteNameIndex(sites, {}) == 2U);

  sites.back().name = "Development";

  CHECK_FALSE(DuplicateSiteNameIndex(sites, {}));
}

TEST_CASE("duplicate site-name detection only compares immediate siblings")
{
  const std::vector sites{
      Site("root", "Production"),
      Site("first", "Production"),
      Site("second", "Production"),
      Site("nested", "Production"),
  };

  std::vector<config::SiteFolder> folders{
      {.id = "work", .name = "Work", .parentId = {}, .siteIds = {"first"}},
      {.id = "personal", .name = "Personal", .parentId = {}, .siteIds = {"second"}},
      {.id = "child", .name = "Child", .parentId = "work", .siteIds = {"nested"}},
  };

  CHECK_FALSE(DuplicateSiteNameIndex(sites, folders));

  SECTION("same-folder duplicate identifies the later site")
  {
    AssignSiteToFolder(folders, "nested", "work");

    CHECK(DuplicateSiteNameIndex(sites, folders) == 3U);
  }

  SECTION("root is also a distinct parent namespace")
  {
    AssignSiteToFolder(folders, "second", {});

    CHECK(DuplicateSiteNameIndex(sites, folders) == 2U);
  }
}

TEST_CASE("new root sites receive the first available numbered default name")
{
  auto sites = std::vector<SiteProfile>{};

  CHECK(NextAvailableSiteNameInTree(sites, {}, {}, "New site") == "New site");

  sites.push_back(Site("first", "New site"));
  sites.push_back(Site("second", "New site 2"));
  sites.push_back(Site("third", "New site 4"));

  CHECK(NextAvailableSiteNameInTree(sites, {}, {}, "New site") == "New site 3");
}

TEST_CASE("site folder assignment moves a site without duplicating it")
{
  std::vector<config::SiteFolder> folders{
      {.id = "work", .name = "Work", .parentId = {}, .siteIds = {"one"}},
      {.id = "test", .name = "Test", .parentId = {}, .siteIds = {"two", "one"}},
  };

  AssignSiteToFolder(folders, "one", "work");

  CHECK(folders[0].siteIds == std::vector<std::string>{"one"});
  CHECK(folders[1].siteIds == std::vector<std::string>{"two"});
  CHECK(SiteFolderId(folders, "one") == "work");

  AssignSiteToFolder(folders, "one", {});

  CHECK(SiteFolderId(folders, "one").empty());
}

TEST_CASE("site folder assignment is safe for aliased ids and unknown destinations")
{
  std::vector<config::SiteFolder> folders{
      {.id = "work", .name = "Work", .parentId = {}, .siteIds = {"one"}},
      {.id = "test", .name = "Test", .parentId = {}, .siteIds = {}},
  };

  const std::string_view aliasedSiteId = folders[0].siteIds[0];

  AssignSiteToFolder(folders, aliasedSiteId, "test");

  CHECK(folders[0].siteIds.empty());
  CHECK(folders[1].siteIds == std::vector<std::string>{"one"});

  AssignSiteToFolder(folders, "one", "missing");

  CHECK(SiteFolderId(folders, "one") == "test");
}

TEST_CASE("folder ancestry includes the folder itself and rejects unrelated branches")
{
  const std::vector<config::SiteFolder> folders{
      {.id = "root", .name = "Root", .parentId = {}, .siteIds = {}},
      {.id = "child", .name = "Child", .parentId = "root", .siteIds = {}},
      {.id = "leaf", .name = "Leaf", .parentId = "child", .siteIds = {}},
      {.id = "other", .name = "Other", .parentId = {}, .siteIds = {}},
  };

  CHECK(IsFolderDescendantOf(folders, "leaf", "root"));
  CHECK(IsFolderDescendantOf(folders, "child", "child"));
  CHECK_FALSE(IsFolderDescendantOf(folders, "other", "root"));
  CHECK_FALSE(IsFolderDescendantOf(folders, {}, "root"));
  CHECK_FALSE(IsFolderDescendantOf(folders, "missing", "missing"));
}

TEST_CASE("folder and site labels cannot collide among visible siblings")
{
  const std::vector sites{
      Site("root-site", "Root site"),
      Site("nested-site", "Nested site"),
  };

  const std::vector<config::SiteFolder> folders{
      {.id = "folder", .name = "Folder", .parentId = {}, .siteIds = {"nested-site"}},
      {.id = "child", .name = "Child", .parentId = "folder", .siteIds = {}},
  };

  CHECK_FALSE(IsFolderNameUnique(folders, sites, "Root site", {}));
  CHECK_FALSE(IsFolderNameUnique(folders, sites, "Nested site", "folder"));
  CHECK(IsFolderNameUnique(folders, sites, "Nested site", {}));
  CHECK_FALSE(IsSiteNameUniqueInTree(sites, folders, "Child", "folder"));
  CHECK(IsSiteNameUniqueInTree(sites, folders, "Child", {}));
  CHECK_FALSE(IsSiteNameUniqueInTree(sites, folders, "Folder", {}));
  CHECK(IsSiteNameUniqueInTree(sites, folders, "Folder", "folder"));
  CHECK(NextAvailableFolderName(folders, sites, {}, "Folder") == "Folder 2");
}

TEST_CASE("site names and generated defaults are scoped to their parent folder")
{
  const std::vector sites{
      Site("one", "Shared"),
      Site("two", "New site"),
      Site("three", "New site 2"),
      Site("four", "New site 4"),
      Site("root", "New site 3"),
  };

  const std::vector<config::SiteFolder> folders{
      {.id = "first", .name = "First", .parentId = {}, .siteIds = {"one"}},
      {.id = "second", .name = "Second", .parentId = {}, .siteIds = {"two", "three", "four"}},
      {.id = "label", .name = "Folder label", .parentId = "second", .siteIds = {}},
  };

  CHECK(IsSiteNameUniqueInTree(sites, folders, "Shared", "second"));
  CHECK(IsSiteNameUniqueInTree(sites, folders, "Shared", {}));
  CHECK_FALSE(IsSiteNameUniqueInTree(sites, folders, "Shared", "first"));
  CHECK_FALSE(IsSiteNameUniqueInTree(sites, folders, "Folder label", "second"));
  CHECK(IsSiteNameUniqueInTree(sites, folders, "Shared", "first", "one"));
  CHECK_FALSE(IsSiteNameUniqueInTree(sites, folders, "Shared", "first", "two"));
  CHECK(NextAvailableSiteNameInTree(sites, folders, "second", "New site") ==
        "New site 3");
  CHECK(NextAvailableSiteNameInTree(sites, folders, "first", "New site") ==
        "New site");
  CHECK(NextAvailableSiteNameInTree(sites, folders, {}, "New site") ==
        "New site");
  CHECK(NextAvailableSiteNameInTree(sites, folders, "second", "Folder label") ==
        "Folder label 2");
}

TEST_CASE("individual site clone suffixes only consider the destination siblings")
{
  const std::vector sites{
      Site("source", "Production"),
      Site("sibling-copy", "Production copy"),
      Site("other-copy", "Production copy 2"),
  };

  const std::vector<config::SiteFolder> folders{
      {.id = "work", .name = "Work", .parentId = {}, .siteIds = {"source", "sibling-copy"}},
      {.id = "personal", .name = "Personal", .parentId = {}, .siteIds = {"other-copy"}},
  };

  CHECK(NextAvailableSiteNameInTree(sites, folders, "work", "Production copy") ==
        "Production copy 2");
  CHECK(NextAvailableSiteNameInTree(sites, folders, "personal", "Production copy") ==
        "Production copy");
}

TEST_CASE("legacy Site Manager order recreates folders-first presentation")
{
  OrderedTreeFixture tree;
  tree.folders[0].siteIds = {"a-two", "a-one"};

  const auto order = SynthesizeSiteManagerOrder(tree.sites, tree.folders);

  CHECK(order == std::vector<std::string>{
                     "a", "a-child", "deep", "a-two", "a-one",
                     "b", "b-one", "root-one", "root-two"});
  CHECK(ChildIds(tree.sites, tree.folders, order, {}) ==
        std::vector<std::string>{"a", "b", "root-one", "root-two"});
  CHECK(ChildIds(tree.sites, tree.folders, order, "a") ==
        std::vector<std::string>{"a-child", "a-two", "a-one"});
}

TEST_CASE("ordered children preserve mixed order and repair missing entries")
{
  const OrderedTreeFixture tree;
  const std::vector<std::string> partial{
      "unknown", "root-one", "a", "root-one", "a-two"};

  CHECK(ChildIds(tree.sites, tree.folders, partial, {}) ==
        std::vector<std::string>{"root-one", "a", "b", "root-two"});
  CHECK(ChildIds(tree.sites, tree.folders, partial, "a") ==
        std::vector<std::string>{"a-two", "a-child", "a-one"});
}

TEST_CASE("Site Manager entries can be freely reordered among mixed siblings")
{
  SECTION("before a root folder")
  {
    OrderedTreeFixture tree;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order,
                               "root-two", "a",
                               SiteManagerDropPosition::Before) ==
          SiteManagerMoveResult::Moved);
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, {}) ==
          std::vector<std::string>{"root-two", "a", "root-one", "b"});
  }

  SECTION("after a root site")
  {
    OrderedTreeFixture tree;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a",
                               "root-one",
                               SiteManagerDropPosition::After) ==
          SiteManagerMoveResult::Moved);
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, {}) ==
          std::vector<std::string>{"root-one", "a", "b", "root-two"});
  }
}

TEST_CASE("Site Manager moves entries across root and nested parents")
{
  SECTION("site moves after a site in another folder")
  {
    OrderedTreeFixture tree;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a-one",
                               "b-one", SiteManagerDropPosition::After) ==
          SiteManagerMoveResult::Moved);
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "a") ==
          std::vector<std::string>{"a-child", "a-two"});
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "b") ==
          std::vector<std::string>{"b-one", "a-one"});
    CHECK(SiteFolderId(tree.folders, "a-one") == "b");
  }

  SECTION("folder moves before a site and retains its subtree")
  {
    OrderedTreeFixture tree;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order,
                               "a-child", "b-one",
                               SiteManagerDropPosition::Before) ==
          SiteManagerMoveResult::Moved);
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "a") ==
          std::vector<std::string>{"a-one", "a-two"});
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "b") ==
          std::vector<std::string>{"a-child", "b-one"});
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "a-child") ==
          std::vector<std::string>{"deep"});
  }

  SECTION("dropping onto a folder appends to its mixed children")
  {
    OrderedTreeFixture tree;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order,
                               "root-one", "a",
                               SiteManagerDropPosition::Into) ==
          SiteManagerMoveResult::Moved);
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, {}) ==
          std::vector<std::string>{"a", "b", "root-two"});
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "a") ==
          std::vector<std::string>{"a-one", "a-child", "a-two",
                                   "root-one"});
  }

  SECTION("dropping onto the current parent moves an entry to the end")
  {
    OrderedTreeFixture tree;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a-one",
                               "a", SiteManagerDropPosition::Into) ==
          SiteManagerMoveResult::Moved);
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "a") ==
          std::vector<std::string>{"a-child", "a-two", "a-one"});
    CHECK(SiteFolderId(tree.folders, "a-one") == "a");
  }

  SECTION("dropping into the root appends there")
  {
    OrderedTreeFixture tree;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "deep",
                               {}, SiteManagerDropPosition::Into) ==
          SiteManagerMoveResult::Moved);
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, {}) ==
          std::vector<std::string>{"a", "root-one", "b", "root-two",
                                   "deep"});
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "a-child").empty());
  }
}

TEST_CASE("Site Manager move no-ops do not rewrite order or membership")
{
  OrderedTreeFixture tree;

  const auto originalFolders = tree.folders;
  const auto originalOrder = tree.order;

  CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order,
                             "root-one", "a",
                             SiteManagerDropPosition::After) ==
        SiteManagerMoveResult::NoChange);
  CHECK(tree.folders == originalFolders);
  CHECK(tree.order == originalOrder);

  CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a-two",
                             "a", SiteManagerDropPosition::Into) ==
        SiteManagerMoveResult::NoChange);
  CHECK(tree.folders == originalFolders);
  CHECK(tree.order == originalOrder);

  CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "b", "b",
                             SiteManagerDropPosition::Before) ==
        SiteManagerMoveResult::NoChange);
  CHECK(tree.folders == originalFolders);
  CHECK(tree.order == originalOrder);
}

TEST_CASE("Site Manager move validation is atomic")
{
  const auto requireRejected = [](OrderedTreeFixture tree,
                                  const std::string_view source,
                                  const std::string_view target,
                                  const SiteManagerDropPosition position,
                                  const SiteManagerMoveResult expected)
  {
    const auto originalFolders = tree.folders;
    const auto originalOrder = tree.order;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, source,
                               target, position) == expected);
    CHECK(tree.folders == originalFolders);
    CHECK(tree.order == originalOrder);
  };

  requireRejected({}, "missing", "a", SiteManagerDropPosition::Before,
                  SiteManagerMoveResult::InvalidSource);
  requireRejected({}, "root-one", "missing",
                  SiteManagerDropPosition::Before,
                  SiteManagerMoveResult::InvalidTarget);
  requireRejected({}, "root-one", "root-two",
                  SiteManagerDropPosition::Into,
                  SiteManagerMoveResult::InvalidTarget);
  requireRejected({}, "root-one", {}, SiteManagerDropPosition::Before,
                  SiteManagerMoveResult::InvalidTarget);
  requireRejected({}, "a", "a-child", SiteManagerDropPosition::Into,
                  SiteManagerMoveResult::Cycle);
  requireRejected({}, "a", "deep", SiteManagerDropPosition::Before,
                  SiteManagerMoveResult::Cycle);
  requireRejected({}, "a", "a-two", SiteManagerDropPosition::After,
                  SiteManagerMoveResult::Cycle);

  OrderedTreeFixture invalidOrder;
  invalidOrder.order.back() = "a";

  requireRejected(std::move(invalidOrder), "root-one", "a",
                  SiteManagerDropPosition::Before,
                  SiteManagerMoveResult::InvalidOrder);
}

TEST_CASE("Site Manager moves reject sibling name collisions")
{
  SECTION("site label collides with a destination site")
  {
    OrderedTreeFixture tree;
    tree.sites.back().name = "A one";

    const auto originalFolders = tree.folders;
    const auto originalOrder = tree.order;

    CHECK_FALSE(DuplicateSiteNameIndex(tree.sites, tree.folders));
    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a-one",
                               "b", SiteManagerDropPosition::Into) ==
          SiteManagerMoveResult::NameConflict);
    CHECK(tree.folders == originalFolders);
    CHECK(tree.order == originalOrder);
  }

  SECTION("site label collides with a root site")
  {
    OrderedTreeFixture tree;
    tree.sites.front().name = "A one";

    const auto originalFolders = tree.folders;
    const auto originalOrder = tree.order;

    CHECK_FALSE(DuplicateSiteNameIndex(tree.sites, tree.folders));
    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a-one",
                               "root-one", SiteManagerDropPosition::Before) ==
          SiteManagerMoveResult::NameConflict);
    CHECK(tree.folders == originalFolders);
    CHECK(tree.order == originalOrder);
  }

  SECTION("site label collides with a destination folder")
  {
    OrderedTreeFixture tree;
    tree.folders.push_back({.id = "same-as-site",
                            .name = "A one",
                            .parentId = "b",
                            .siteIds = {}});
    tree.order.push_back("same-as-site");

    const auto originalFolders = tree.folders;
    const auto originalOrder = tree.order;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a-one",
                               "b", SiteManagerDropPosition::Into) ==
          SiteManagerMoveResult::NameConflict);
    CHECK(tree.folders == originalFolders);
    CHECK(tree.order == originalOrder);
  }

  SECTION("folder label collides with a destination site")
  {
    OrderedTreeFixture tree;
    tree.folders.push_back({.id = "same-as-site",
                            .name = "B one",
                            .parentId = "a",
                            .siteIds = {}});
    tree.order.push_back("same-as-site");

    const auto originalFolders = tree.folders;
    const auto originalOrder = tree.order;

    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order,
                               "same-as-site", "b",
                               SiteManagerDropPosition::Into) ==
          SiteManagerMoveResult::NameConflict);
    CHECK(tree.folders == originalFolders);
    CHECK(tree.order == originalOrder);
  }
}

TEST_CASE("Site Manager moves ignore matching names outside the destination")
{
  OrderedTreeFixture tree;
  tree.sites.front().name = "A one";
  tree.sites.back().name = "A one";

  SECTION("moving to a different nested parent")
  {
    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a-one",
                               "a-child", SiteManagerDropPosition::Into) ==
          SiteManagerMoveResult::Moved);
    CHECK(SiteFolderId(tree.folders, "a-one") == "a-child");
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "a-child") ==
          std::vector<std::string>{"deep", "a-one"});
    CHECK_FALSE(DuplicateSiteNameIndex(tree.sites, tree.folders));
  }

  SECTION("reordering within the same folder")
  {
    CHECK(MoveSiteManagerEntry(tree.sites, tree.folders, tree.order, "a-one",
                               "a", SiteManagerDropPosition::Into) ==
          SiteManagerMoveResult::Moved);
    CHECK(SiteFolderId(tree.folders, "a-one") == "a");
    CHECK(ChildIds(tree.sites, tree.folders, tree.order, "a") ==
          std::vector<std::string>{"a-child", "a-two", "a-one"});
    CHECK_FALSE(DuplicateSiteNameIndex(tree.sites, tree.folders));
  }
}

TEST_CASE("site clones keep connection settings without sharing credentials")
{
  auto source = Site("source", "Production");
  source.host = "example.test";
  source.protocol = ProtocolKind::Sftp;
  source.port = 2222;
  source.username = "alice";
  source.authentication.kind = AuthenticationKind::PrivateKey;
  source.authentication.credentialId = "source-password";
  source.authentication.privateKeyFile = L"C:\\Keys\\id_ed25519";
  source.authentication.publicKeyFile = L"C:\\Keys\\id_ed25519.pub";
  source.authentication.passphraseCredentialId = "source-passphrase";
  source.initialLocalDirectory = L"C:\\Uploads";
  source.initialRemoteDirectory = RemotePath{"/srv/uploads"};
  source.ftpEncoding = "ISO-8859-1";

  const auto clone = CloneSiteProfile(source, "clone", "Production copy");

  CHECK(clone.id == "clone");
  CHECK(clone.name == "Production copy");
  CHECK(clone.host == source.host);
  CHECK(clone.protocol == source.protocol);
  CHECK(clone.port == source.port);
  CHECK(clone.username == source.username);
  CHECK(clone.authentication.kind == source.authentication.kind);
  CHECK(clone.authentication.privateKeyFile ==
        source.authentication.privateKeyFile);
  CHECK(clone.authentication.publicKeyFile ==
        source.authentication.publicKeyFile);
  CHECK(clone.initialLocalDirectory == source.initialLocalDirectory);
  CHECK(clone.initialRemoteDirectory == source.initialRemoteDirectory);
  CHECK(clone.ftpEncoding == source.ftpEncoding);
  CHECK(clone.authentication.credentialId.empty());
  CHECK(clone.authentication.passphraseCredentialId.empty());
  CHECK(source.authentication.credentialId == "source-password");
}

TEST_CASE("folder tree clones preserve hierarchy and mixed subtree order")
{
  auto first = Site("first", "Production");
  first.host = "production.example.test";
  first.authentication.credentialId = "production-password";
  first.authentication.passphraseCredentialId = "production-passphrase";

  auto second = Site("second", "Staging");
  second.host = "staging.example.test";
  second.authentication.credentialId = "staging-password";

  auto nested = Site("nested", "Archive");
  nested.host = "archive.example.test";
  nested.authentication.passphraseCredentialId = "archive-passphrase";

  const std::vector sites{
      first,
      second,
      nested,
      Site("existing-production-copy", "Production copy"),
      Site("existing-staging-copy", "Staging copy"),
      Site("existing-staging-copy-2", "Staging copy 2"),
  };

  const std::vector<config::SiteFolder> folders{
      {.id = "parent", .name = "Accounts", .parentId = {}, .siteIds = {}},
      {.id = "source", .name = "Servers", .parentId = "parent", .siteIds = {"second", "first"}},
      {.id = "child", .name = "Old", .parentId = "source", .siteIds = {"nested"}},
      {.id = "leaf", .name = "Empty leaf", .parentId = "child", .siteIds = {}},
      {.id = "sibling-child", .name = "Other", .parentId = "source", .siteIds = {}},
      {.id = "existing-copy", .name = "Servers copy", .parentId = "parent", .siteIds = {}},
  };

  const auto originalFolders = folders;

  const std::vector<std::string> sourceOrder{
      "parent", "source", "second", "child", "first",
      "sibling-child", "nested", "leaf", "existing-copy",
      "existing-production-copy", "existing-staging-copy",
      "existing-staging-copy-2"};

  const auto cloned = CloneSiteFolderTree(
      sites, folders, "source", " copy", sourceOrder);

  REQUIRE(cloned);
  REQUIRE(cloned->folders.size() == 4U);
  REQUIRE(cloned->sites.size() == 3U);

  const auto &root = cloned->folders[0];
  const auto &child = cloned->folders[1];
  const auto &leaf = cloned->folders[2];
  const auto &siblingChild = cloned->folders[3];

  CHECK(cloned->rootFolderId == root.id);
  CHECK(root.name == "Servers copy 2");
  CHECK(root.parentId == "parent");
  CHECK(child.name == "Old");
  CHECK(child.parentId == root.id);
  CHECK(leaf.name == "Empty leaf");
  CHECK(leaf.parentId == child.id);
  CHECK(siblingChild.name == "Other");
  CHECK(siblingChild.parentId == root.id);
  CHECK(leaf.siteIds.empty());
  CHECK(siblingChild.siteIds.empty());

  REQUIRE(root.siteIds.size() == 2U);
  CHECK(root.siteIds[0] == cloned->sites[0].id);
  CHECK(root.siteIds[1] == cloned->sites[1].id);
  CHECK(cloned->sites[0].host == "staging.example.test");
  CHECK(cloned->sites[0].name == "Staging");
  CHECK(cloned->sites[1].host == "production.example.test");
  CHECK(cloned->sites[1].name == "Production");
  REQUIRE(child.siteIds.size() == 1U);
  CHECK(child.siteIds.front() == cloned->sites[2].id);
  CHECK(cloned->sites[2].host == "archive.example.test");
  CHECK(cloned->sites[2].name == "Archive");
  CHECK(cloned->siteManagerOrder ==
        std::vector<std::string>{
            root.id, cloned->sites[0].id, child.id, cloned->sites[1].id,
            siblingChild.id, cloned->sites[2].id, leaf.id});

  std::unordered_set<std::string> allIds;

  for (const auto &original : sites)
  {
    allIds.insert(original.id);
  }

  for (const auto &original : folders)
  {
    allIds.insert(original.id);
  }

  for (const auto &folder : cloned->folders)
  {
    CHECK_FALSE(folder.id.empty());
    CHECK(allIds.insert(folder.id).second);
  }

  for (const auto &clonedSite : cloned->sites)
  {
    CHECK_FALSE(clonedSite.id.empty());
    CHECK(allIds.insert(clonedSite.id).second);
    CHECK(clonedSite.authentication.credentialId.empty());
    CHECK(clonedSite.authentication.passphraseCredentialId.empty());
  }

  auto combinedSites = sites;
  combinedSites.insert(combinedSites.end(), cloned->sites.begin(), cloned->sites.end());

  auto combinedFolders = folders;
  combinedFolders.insert(combinedFolders.end(), cloned->folders.begin(), cloned->folders.end());

  CHECK_FALSE(DuplicateSiteNameIndex(combinedSites, combinedFolders));

  CHECK(folders == originalFolders);
  CHECK(sites[0].id == "first");
  CHECK(sites[0].name == "Production");
  CHECK(sites[0].authentication.credentialId == "production-password");
  CHECK(sites[0].authentication.passphraseCredentialId ==
        "production-passphrase");
}

TEST_CASE("folder tree clones retain equal site names in different descendant parents")
{
  const std::vector sites{
      Site("first", "Production"),
      Site("second", "Production"),
  };

  const std::vector<config::SiteFolder> folders{
      {.id = "source", .name = "Servers", .parentId = {}, .siteIds = {"first"}},
      {.id = "child", .name = "Child", .parentId = "source", .siteIds = {"second"}},
  };

  const auto cloned = CloneSiteFolderTree(sites, folders, "source", " copy");

  REQUIRE(cloned);
  REQUIRE(cloned->folders.size() == 2U);
  REQUIRE(cloned->sites.size() == 2U);
  CHECK(cloned->folders[0].name == "Servers copy");
  CHECK(cloned->folders[1].name == "Child");
  CHECK(cloned->sites[0].name == "Production");
  CHECK(cloned->sites[1].name == "Production");
  CHECK(cloned->sites[0].id != cloned->sites[1].id);
  CHECK(cloned->sites[0].id != sites[0].id);
  CHECK(cloned->sites[1].id != sites[1].id);
  CHECK(SiteFolderId(cloned->folders, cloned->sites[0].id) == cloned->folders[0].id);
  CHECK(SiteFolderId(cloned->folders, cloned->sites[1].id) == cloned->folders[1].id);
  CHECK_FALSE(DuplicateSiteNameIndex(cloned->sites, cloned->folders));
}

TEST_CASE("empty folders can be cloned beside their source")
{
  const std::vector<SiteProfile> sites;

  const std::vector<config::SiteFolder> folders{
      {.id = "parent", .name = "Parent", .parentId = {}, .siteIds = {}},
      {.id = "empty", .name = "Empty", .parentId = "parent", .siteIds = {}},
  };

  const auto cloned = CloneSiteFolderTree(sites, folders, "empty", " copy");

  REQUIRE(cloned);
  REQUIRE(cloned->folders.size() == 1U);
  CHECK(cloned->rootFolderId == cloned->folders.front().id);
  CHECK(cloned->folders.front().name == "Empty copy");
  CHECK(cloned->folders.front().parentId == "parent");
  CHECK(cloned->folders.front().siteIds.empty());
  CHECK(cloned->sites.empty());
}

TEST_CASE("folder tree cloning rejects a missing source")
{
  const std::vector<SiteProfile> sites;

  const std::vector<config::SiteFolder> folders{
      {.id = "present", .name = "Present", .parentId = {}, .siteIds = {}},
  };

  CHECK_FALSE(CloneSiteFolderTree(sites, folders, "missing", " copy"));
}

TEST_CASE("site imports append mixed roots to the destination and retain nested order")
{
  config::SiteTransferData existing{
      .sites = {Site("existing", "Existing")},
      .folders = {{.id = "destination", .name = "Destination", .parentId = {}, .siteIds = {"existing"}}},
      .siteManagerOrder = {"destination", "existing"}};
  existing.sites.front().host = "unchanged.test";
  existing.sites.front().authentication.credentialId = "existing-password";

  const auto oldFolders = existing.folders;
  const auto oldOrder = existing.siteManagerOrder;

  const OrderedTreeFixture fixture;
  const config::SiteTransferData incoming{fixture.sites, fixture.folders, fixture.order};

  const auto plan = PlanSiteImport(existing, incoming, "destination");

  REQUIRE(plan);
  CHECK(plan->renames.empty());
  CHECK(ChildIds(plan->merged.sites, plan->merged.folders,
                 plan->merged.siteManagerOrder, {}) ==
        std::vector<std::string>{"destination"});
  CHECK(ChildIds(plan->merged.sites, plan->merged.folders,
                 plan->merged.siteManagerOrder, "destination") ==
        std::vector<std::string>{"existing", "a", "root-one", "b", "root-two"});
  CHECK(ChildIds(plan->merged.sites, plan->merged.folders,
                 plan->merged.siteManagerOrder, "a") ==
        std::vector<std::string>{"a-one", "a-child", "a-two"});
  CHECK(ChildIds(plan->merged.sites, plan->merged.folders,
                 plan->merged.siteManagerOrder, "a-child") ==
        std::vector<std::string>{"deep"});
  CHECK(plan->merged.sites.front().host == "unchanged.test");
  CHECK(plan->merged.sites.front().authentication.credentialId == "existing-password");
  CHECK(existing.folders == oldFolders);
  CHECK(existing.siteManagerOrder == oldOrder);
  CHECK(incoming.folders == fixture.folders);
  CHECK(incoming.siteManagerOrder == fixture.order);
}

TEST_CASE("site import proposes keep-both names for site and folder collisions")
{
  const config::SiteTransferData existing{
      .sites = {Site("first", "Production"), Site("numbered", "Production 2")},
      .folders = {{.id = "folder", .name = "Servers", .parentId = {}, .siteIds = {}}},
      .siteManagerOrder = {"first", "folder", "numbered"}};

  const config::SiteTransferData incoming{
      .sites = {Site("import-site", "Servers"), Site("nested-site", "Production")},
      .folders = {{.id = "import-folder", .name = "Production", .parentId = {}, .siteIds = {"nested-site"}}},
      .siteManagerOrder = {"import-folder", "import-site", "nested-site"}};

  const auto plan = PlanSiteImport(existing, incoming, {});

  REQUIRE(plan);
  CHECK(plan->renames == std::vector<SiteManagerImportRename>{
                             {"import-folder", "Production", "Production 3"},
                             {"import-site", "Servers", "Servers 2"}});
  CHECK(plan->merged.folders.back().name == "Production 3");
  CHECK(plan->merged.sites[2].name == "Servers 2");
  CHECK(plan->merged.sites[3].name == "Production");
  CHECK(ChildIds(plan->merged.sites, plan->merged.folders,
                 plan->merged.siteManagerOrder, {}) ==
        std::vector<std::string>{"first", "folder", "numbered", "import-folder", "import-site"});
  CHECK(incoming.folders.front().name == "Production");
  CHECK(incoming.sites.front().name == "Servers");
}

TEST_CASE("site imports only resolve name collisions in the selected parent")
{
  const config::SiteTransferData existing{
      .sites = {Site("existing", "Production")},
      .folders = {{.id = "destination", .name = "Destination", .parentId = {}, .siteIds = {}},
                  {.id = "other", .name = "Other", .parentId = {}, .siteIds = {"existing"}}},
      .siteManagerOrder = {"destination", "other", "existing"}};

  config::SiteTransferData incoming{
      .sites = {Site("imported", "Production")},
      .folders = {},
      .siteManagerOrder = {"imported"}};

  incoming.sites.front().authentication.kind = AuthenticationKind::PrivateKey;

  const auto plan = PlanSiteImport(existing, incoming, "destination");

  REQUIRE(plan);
  CHECK(plan->renames.empty());
  CHECK(SiteFolderId(plan->merged.folders, "imported") == "destination");
  CHECK(plan->merged.sites.back().name == "Production");
  CHECK(plan->merged.sites.back().authentication.kind == AuthenticationKind::PrivateKey);
  CHECK(plan->merged.sites.back().authentication.privateKeyFile.empty());
}

TEST_CASE("site import proposals do not consume another incoming entry's unchanged name")
{
  const config::SiteTransferData existing{
      .sites = {Site("existing", "Production")}, .folders = {}, .siteManagerOrder = {"existing"}};

  const config::SiteTransferData incoming{
      .sites = {Site("imported", "Production"), Site("numbered", "Production 2")},
      .folders = {},
      .siteManagerOrder = {"imported", "numbered"}};

  const auto plan = PlanSiteImport(existing, incoming, {});

  REQUIRE(plan);
  CHECK(plan->renames == std::vector<SiteManagerImportRename>{
                             {"imported", "Production", "Production 3"}});
  CHECK(plan->merged.sites.back().name == "Production 2");
}

TEST_CASE("discarding a site import preview leaves existing sites and folders unchanged")
{
  config::SiteTransferData existing{
      .sites = {Site("existing", "Production")},
      .folders = {{.id = "destination", .name = "Destination", .parentId = {}, .siteIds = {"existing"}}},
      .siteManagerOrder = {"destination", "existing"}};

  const config::SiteTransferData incoming{
      .sites = {Site("imported", "Production")},
      .folders = {},
      .siteManagerOrder = {"imported"}};

  const auto oldFolders = existing.folders;
  const auto oldOrder = existing.siteManagerOrder;

  {
    const auto preview = PlanSiteImport(existing, incoming, "destination");

    REQUIRE(preview);
    REQUIRE(preview->renames.size() == 1);
    CHECK(preview->renames.front().proposedName == "Production 2");
  }

  REQUIRE(existing.sites.size() == 1);
  CHECK(existing.sites.front().id == "existing");
  CHECK(existing.sites.front().name == "Production");
  CHECK(existing.folders == oldFolders);
  CHECK(existing.siteManagerOrder == oldOrder);
}

TEST_CASE("site imports reject existing identity collisions across entry kinds")
{
  const config::SiteTransferData existing{
      .sites = {Site("shared-id", "Original")},
      .folders = {},
      .siteManagerOrder = {"shared-id"}};

  const config::SiteTransferData incoming{
      .sites = {},
      .folders = {{.id = "shared-id", .name = "Imported folder", .parentId = {}, .siteIds = {}}},
      .siteManagerOrder = {"shared-id"}};

  const auto plan = PlanSiteImport(existing, incoming, {});

  REQUIRE_FALSE(plan);
  CHECK(plan.error() == SiteManagerImportError::IdentityCollision);
  CHECK(existing.sites.front().name == "Original");
  CHECK(existing.folders.empty());
}

TEST_CASE("site import rejects malformed trees and missing destinations transactionally")
{
  const config::SiteTransferData existing{
      .sites = {Site("existing", "Existing")},
      .folders = {},
      .siteManagerOrder = {"existing"}};

  config::SiteTransferData incoming{
      .sites = {Site("imported", "Imported")},
      .folders = {{.id = "folder", .name = "Folder", .parentId = {}, .siteIds = {"imported"}}},
      .siteManagerOrder = {"folder", "imported"}};

  std::string destination;

  auto expectedError = SiteManagerImportError::InvalidTree;

  SECTION("unknown destination")
  {
    destination = "missing";
    expectedError = SiteManagerImportError::InvalidDestination;
  }
  SECTION("incomplete order") { incoming.siteManagerOrder.pop_back(); }
  SECTION("duplicate id")
  {
    incoming.folders.front().id = "imported";
    incoming.siteManagerOrder.front() = "imported";
  }
  SECTION("dangling parent") { incoming.folders.front().parentId = "missing"; }
  SECTION("cycle") { incoming.folders.front().parentId = "folder"; }
  SECTION("dangling site assignment") { incoming.folders.front().siteIds.push_back("missing"); }
  SECTION("duplicate site assignment") { incoming.folders.front().siteIds.push_back("imported"); }
  SECTION("duplicate sibling names")
  {
    incoming.folders.front().siteIds.clear();
    incoming.sites.front().name = "Folder";
  }

  const auto plan = PlanSiteImport(existing, incoming, destination);

  REQUIRE_FALSE(plan);
  CHECK(plan.error() == expectedError);
  REQUIRE(existing.sites.size() == 1);
  CHECK(existing.sites.front().id == "existing");
  CHECK(existing.sites.front().name == "Existing");
  CHECK(existing.folders.empty());
  CHECK(existing.siteManagerOrder == std::vector<std::string>{"existing"});
}
