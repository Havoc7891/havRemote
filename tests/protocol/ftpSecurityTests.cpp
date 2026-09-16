// SPDX-License-Identifier: MIT

#include "protocol/ftpSession.hpp"
#include "protocol/sftpSession.hpp"

#include <catch2/catch_test_macros.hpp>
#include <curl/curl.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

using namespace havremote;

TEST_CASE("the network backend supports TLS for FTP and update requests", "[tls-backend]")
{
  const auto *version = curl_version_info(CURLVERSION_NOW);

  REQUIRE(version != nullptr);
  CHECK((version->features & CURL_VERSION_SSL) != 0);
  REQUIRE(version->ssl_version != nullptr);
  CHECK_FALSE(std::string_view{version->ssl_version}.empty());
#if defined(_WIN32)
  CHECK(std::string_view{version->ssl_version}.starts_with("LibreSSL/"));
#endif
  REQUIRE(version->protocols != nullptr);

  for (const std::string_view required : {"ftp", "ftps", "http", "https"})
  {
    CAPTURE(required);

    bool found{};

    for (const char *const *protocol = version->protocols; *protocol; ++protocol)
    {
      found = found || required == *protocol;
    }

    CHECK(found);
  }
}

TEST_CASE("FTP endpoint hosts are validated before URL construction")
{
  const auto dns = ftp::FormatHostForUrl("files.example.test");

  REQUIRE(dns);
  CHECK(*dns == "files.example.test");

  const auto absoluteDns = ftp::FormatHostForUrl("files.example.test.");

  REQUIRE(absoluteDns);
  CHECK(*absoluteDns == "files.example.test.");

  const auto ipv4 = ftp::FormatHostForUrl("192.0.2.7");

  REQUIRE(ipv4);
  CHECK(*ipv4 == "192.0.2.7");

  const auto ipv6 = ftp::FormatHostForUrl("2001:db8::7");

  REQUIRE(ipv6);
  CHECK(*ipv6 == "[2001:db8::7]");

  constexpr std::array rejected{
      "ftp://trusted.example",
      "user@trusted.example",
      "trusted.example/path",
      "trusted.example\\path",
      "trusted.example:21",
      "[2001:db8::7]",
      "::1:",
      "1:2:3:4:5:6:7:8:",
      "trusted.example?mode=passive",
      "trusted.example#fragment",
      "trusted%2eexample",
      "trusted.example\r\nINJECT",
      "bad_label.example",
      "-bad.example",
      "bad-.example",
      "bad..example",
      "999.999.999.999",
      "12345",
      "m\xc3\xbcnchen.example",
  };

  for (const std::string_view host : rejected)
  {
    INFO("host=" << host);

    const auto result = ftp::FormatHostForUrl(host);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == RemoteErrorCode::InvalidArgument);
  }
}

TEST_CASE("active FTP uses the control socket route unless explicitly overridden")
{
  const auto automatic = ftp::MakeActiveModePortSpecification("");

  REQUIRE(automatic);
  CHECK(*automatic == "-");

  const auto ipv4 =
      ftp::MakeActiveModePortSpecification("198.51.100.24");

  REQUIRE(ipv4);
  CHECK(*ipv4 == "198.51.100.24");

  const auto ipv6 =
      ftp::MakeActiveModePortSpecification("2001:db8::24");

  REQUIRE(ipv6);
  CHECK(*ipv6 == "2001:db8::24");

  // Loopback is never inferred from the server hostname. It can be supplied
  // explicitly for a genuinely local server. Automatic mode may also select it
  // when getsockname() reports it as the control socket's actual local address.
  const auto explicitLoopback =
      ftp::MakeActiveModePortSpecification("127.0.0.1");

  REQUIRE(explicitLoopback);
  CHECK(*explicitLoopback == "127.0.0.1");

  constexpr std::array rejected{
      "localhost",
      "files.example.test",
      "[2001:db8::24]",
      "198.51.100.24:50000",
      "fe80::1%12",
      " 198.51.100.24",
      "198.51.100.24 ",
      "ftp://198.51.100.24",
  };

  for (const std::string_view address : rejected)
  {
    INFO("address=" << address);

    const auto result = ftp::MakeActiveModePortSpecification(address);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == RemoteErrorCode::InvalidArgument);
  }
}

TEST_CASE("active FTP local address checks reject non-addresses and unassigned addresses")
{
  const auto invalid = ftp::IsAssignedLocalAddress("localhost");

  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().code == RemoteErrorCode::InvalidArgument);

  const auto unspecified = ftp::IsAssignedLocalAddress("0.0.0.0");

  INFO((unspecified ? "Address enumeration succeeded" : unspecified.error().message));
  REQUIRE(unspecified);
  CHECK_FALSE(*unspecified);

  const auto loopback = ftp::IsAssignedLocalAddress("127.0.0.1");

  REQUIRE(loopback);
  CHECK(*loopback);
}

TEST_CASE("FTP command paths remain relative to the login directory")
{
  const auto logicalAbsolute =
      ftp::MakeLoginRelativeCommandPath("/test/file.txt");

  REQUIRE(logicalAbsolute);
  CHECK(*logicalAbsolute == "test/file.txt");

  const auto alreadyRelative =
      ftp::MakeLoginRelativeCommandPath("test/file.txt");

  REQUIRE(alreadyRelative);
  CHECK(*alreadyRelative == "test/file.txt");

  // Only the URL-authority separator is removed. Further leading separators
  // deliberately retain libcurl's server-filesystem-absolute URL semantics.
  const auto repeatedLeadingSeparators =
      ftp::MakeLoginRelativeCommandPath("///test/file.txt");

  REQUIRE(repeatedLeadingSeparators);
  CHECK(*repeatedLeadingSeparators == "//test/file.txt");

  std::string legacyEncodedPath{"/Gr"};
  legacyEncodedPath.push_back(static_cast<char>(0xFC));
  legacyEncodedPath += "e.txt";

  const auto legacyEncoded =
      ftp::MakeLoginRelativeCommandPath(legacyEncodedPath);

  REQUIRE(legacyEncoded);
  CHECK(*legacyEncoded == legacyEncodedPath.substr(1));

  for (const std::string &unsafe : {
           std::string{},
           std::string{"/"},
           std::string{"//"},
           std::string{"////"},
           std::string{"test/file.txt\rRNTO injected"},
           std::string{"test/file.txt\nRNTO injected"},
           std::string{"test\0file.txt", 13U},
       })
  {
    const auto result = ftp::MakeLoginRelativeCommandPath(unsafe);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == RemoteErrorCode::InvalidArgument);
  }
}

TEST_CASE("FTP data connection classification excludes rename and control commands")
{
  CHECK(ftp::UsesDataConnection(ftp::OperationKind::Nlst));
  CHECK(ftp::UsesDataConnection(ftp::OperationKind::Mlsd));
  CHECK(ftp::UsesDataConnection(ftp::OperationKind::List));
  CHECK(ftp::UsesDataConnection(ftp::OperationKind::Retr));
  CHECK(ftp::UsesDataConnection(ftp::OperationKind::Stor));

  CHECK_FALSE(ftp::UsesDataConnection(ftp::OperationKind::Rename));
  CHECK_FALSE(ftp::UsesDataConnection(ftp::OperationKind::ControlCommand));
}

TEST_CASE("direct protocol callers cannot bypass endpoint host validation")
{
  SiteProfile ftpSite;
  ftpSite.id = "invalid-ftp";
  ftpSite.protocol = ProtocolKind::FtpsExplicit;
  ftpSite.host = "trusted.example@127.0.0.1";
  ftpSite.port = 21;
  ftpSite.username = "user";
  ftpSite.authentication.kind = AuthenticationKind::Password;

  int credentialRequests{};

  SessionCallbacks callbacks;

  callbacks.requestCredential = [&](const CredentialRequest &, std::stop_token)
  {
    ++credentialRequests;

    return Result<std::string>{"secret"};
  };

  const auto ftpSession = MakeFtpSession();

  const auto ftpResult = ftpSession->Connect(ftpSite, callbacks, {});

  REQUIRE_FALSE(ftpResult);
  CHECK(ftpResult.error().code == RemoteErrorCode::InvalidArgument);
  CHECK(credentialRequests == 0);

  auto sftpSite = ftpSite;
  sftpSite.id = "invalid-sftp";
  sftpSite.protocol = ProtocolKind::Sftp;
  sftpSite.port = 22;

  const auto sftpSession = MakeSftpSession();
  const auto sftpResult = sftpSession->Connect(sftpSite, callbacks, {});

  REQUIRE_FALSE(sftpResult);
  CHECK(sftpResult.error().code == RemoteErrorCode::InvalidArgument);
  CHECK(credentialRequests == 0);
}

TEST_CASE("direct FTP callers cannot supply an invalid data connection mode")
{
  SiteProfile site;
  site.id = "invalid-ftp-data-mode";
  site.protocol = ProtocolKind::Ftp;
  site.host = "files.example.test";
  site.port = 21;
  site.username = "user";
  site.authentication.kind = AuthenticationKind::Password;
  site.ftpDataConnectionMode = static_cast<FtpDataConnectionMode>(99);

  int credentialRequests{};

  SessionCallbacks callbacks;

  callbacks.requestCredential = [&](const CredentialRequest &, std::stop_token)
  {
    ++credentialRequests;

    return Result<std::string>{"secret"};
  };

  const auto session = MakeFtpSession();
  const auto result = session->Connect(site, callbacks, {});

  REQUIRE_FALSE(result);
  CHECK(result.error().code == RemoteErrorCode::InvalidArgument);
  CHECK(credentialRequests == 0);
}

TEST_CASE("direct FTP callers cannot supply a hostname as an active address")
{
  SiteProfile site;
  site.id = "invalid-active-address";
  site.protocol = ProtocolKind::Ftp;
  site.host = "files.example.test";
  site.port = 21;
  site.username = "user";
  site.authentication.kind = AuthenticationKind::Password;
  site.ftpDataConnectionMode = FtpDataConnectionMode::Active;
  site.ftpActiveAddress = "localhost";

  int credentialRequests{};

  SessionCallbacks callbacks;

  callbacks.requestCredential = [&](const CredentialRequest &, std::stop_token)
  {
    ++credentialRequests;

    return Result<std::string>{"secret"};
  };

  const auto session = MakeFtpSession();
  const auto result = session->Connect(site, callbacks, {});

  REQUIRE_FALSE(result);
  CHECK(result.error().code == RemoteErrorCode::InvalidArgument);
  CHECK(credentialRequests == 0);
}

TEST_CASE("direct FTP callers cannot use a non-local active address")
{
  SiteProfile site;
  site.id = "non-local-active-address";
  site.protocol = ProtocolKind::Ftp;
  site.host = "files.example.test";
  site.port = 21;
  site.username = "user";
  site.authentication.kind = AuthenticationKind::Password;
  site.ftpDataConnectionMode = FtpDataConnectionMode::Active;

  // The unspecified address is syntactically valid but can never be assigned
  // as a unicast address to an active interface.
  site.ftpActiveAddress = "0.0.0.0";

  int credentialRequests{};

  SessionCallbacks callbacks;

  callbacks.requestCredential = [&](const CredentialRequest &, std::stop_token)
  {
    ++credentialRequests;

    return Result<std::string>{"secret"};
  };

  const auto session = MakeFtpSession();
  const auto result = session->Connect(site, callbacks, {});

  REQUIRE_FALSE(result);
  CHECK(result.error().code == RemoteErrorCode::InvalidArgument);
  CHECK(result.error().message.find("not assigned") != std::string::npos);
  CHECK(credentialRequests == 0);
}

TEST_CASE("FTPS untrusted issuer diagnostics support LibreSSL OpenSSL and Schannel")
{
  for (const std::string_view diagnostic : {
           "schannel: SEC_E_UNTRUSTED_ROOT (0x80090325) - The certificate chain was issued by an authority that is not trusted.",
           "schannel: SEC_E_UNTRUSTED_ROOT (0x80090325) - Die Zertifikatkette wurde von einer nicht vertrauenswuerdigen Zertifizierungsstelle ausgestellt.",
           "schannel: certificate chain is incomplete",
           "schannel: certificate chain was based on an untrusted root",
           "SSL certificate problem: self-signed certificate",
           "SSL certificate problem: self signed certificate",
           "SSL certificate OpenSSL verify result: self signed certificate (18)",
           "SSL certificate problem: unable to get local issuer certificate",
           "SSL certificate problem: unable to verify the first certificate",
       })
  {
    CAPTURE(diagnostic);
    CHECK(ftp::ClassifyTlsVerificationFailure(diagnostic) ==
          ftp::TlsVerificationFailure::UntrustedIssuer);
  }
}

TEST_CASE("FTPS issuer diagnostics do not override other certificate defects")
{
  constexpr std::array cases{
      std::pair{"certificate revoked", ftp::TlsVerificationFailure::Revoked},
      std::pair{"certificate not time valid", ftp::TlsVerificationFailure::ExpiredOrNotYetValid},
      std::pair{"certificate expired", ftp::TlsVerificationFailure::ExpiredOrNotYetValid},
      std::pair{"certificate not yet valid", ftp::TlsVerificationFailure::ExpiredOrNotYetValid},
      std::pair{"hostname mismatch", ftp::TlsVerificationFailure::HostnameMismatch},
      std::pair{"certificate does not match", ftp::TlsVerificationFailure::HostnameMismatch},
      std::pair{"CERT_E_CN_NO_MATCH", ftp::TlsVerificationFailure::HostnameMismatch},
  };

  for (const auto &[diagnostic, expected] : cases)
  {
    CAPTURE(diagnostic);
    CHECK(ftp::ClassifyTlsVerificationFailure(diagnostic) == expected);
    CHECK(ftp::ClassifyTlsVerificationFailure(
              std::string{"schannel: SEC_E_UNTRUSTED_ROOT, "} + diagnostic) == expected);
    CHECK(ftp::ClassifyTlsVerificationFailure(
              std::string{"SSL certificate OpenSSL verify result: self signed certificate (18), "} +
              diagnostic) == expected);
  }

  for (const std::string_view diagnostic : {
           "", "SSL certificate verification failed", "schannel: SEC_E_ILLEGAL_MESSAGE",
           "certificate authority is not trusted",
       })
  {
    CAPTURE(diagnostic);
    CHECK(ftp::ClassifyTlsVerificationFailure(diagnostic) ==
          ftp::TlsVerificationFailure::Other);
  }
}

TEST_CASE("FTPS trust decisions require explicit replacement for changed pins")
{
  CHECK(ftp::IsTlsTrustDecisionAllowed(TrustStatus::Invalid,
                                       TrustDecision::AcceptOnce));
  CHECK(ftp::IsTlsTrustDecisionAllowed(TrustStatus::Invalid,
                                       TrustDecision::AcceptPermanently));
  CHECK_FALSE(ftp::IsTlsTrustDecisionAllowed(TrustStatus::Invalid,
                                             TrustDecision::ReplaceStored));

  CHECK(ftp::IsTlsTrustDecisionAllowed(TrustStatus::Changed,
                                       TrustDecision::ReplaceStored));
  CHECK_FALSE(ftp::IsTlsTrustDecisionAllowed(TrustStatus::Changed,
                                             TrustDecision::AcceptOnce));
  CHECK_FALSE(ftp::IsTlsTrustDecisionAllowed(TrustStatus::Changed,
                                             TrustDecision::AcceptPermanently));
  CHECK_FALSE(ftp::IsTlsTrustDecisionAllowed(TrustStatus::Changed,
                                             TrustDecision::Reject));
}

TEST_CASE("FTPS PIN exceptions are limited to valid genuinely self-signed leaves")
{
  ftp::TlsPublicKeyIdentity identity;
  identity.genuinelySelfSigned = true;
  identity.currentlyTimeValid = true;
  identity.serverAuthenticationValidExceptTrustAnchor = true;

  CHECK(ftp::IsTlsPinExceptionEligible(
      identity, ftp::TlsVerificationFailure::UntrustedIssuer));
  CHECK_FALSE(ftp::IsTlsPinExceptionEligible(
      identity, ftp::TlsVerificationFailure::ExpiredOrNotYetValid));
  CHECK_FALSE(ftp::IsTlsPinExceptionEligible(
      identity, ftp::TlsVerificationFailure::Revoked));
  CHECK_FALSE(ftp::IsTlsPinExceptionEligible(
      identity, ftp::TlsVerificationFailure::HostnameMismatch));
  CHECK_FALSE(ftp::IsTlsPinExceptionEligible(
      identity, ftp::TlsVerificationFailure::Other));

  identity.currentlyTimeValid = false;

  CHECK_FALSE(ftp::IsTlsPinExceptionEligible(
      identity, ftp::TlsVerificationFailure::UntrustedIssuer));

  identity.currentlyTimeValid = true;
  identity.genuinelySelfSigned = false;

  CHECK_FALSE(ftp::IsTlsPinExceptionEligible(
      identity, ftp::TlsVerificationFailure::UntrustedIssuer));

  identity.genuinelySelfSigned = true;
  identity.serverAuthenticationValidExceptTrustAnchor = false;

  CHECK_FALSE(ftp::IsTlsPinExceptionEligible(
      identity, ftp::TlsVerificationFailure::UntrustedIssuer));
}

TEST_CASE("FTPS certificate inspection fingerprints the exact leaf SPKI")
{
  constexpr std::string_view certificate = R"CERT(-----BEGIN CERTIFICATE-----
MIIDcTCCAlmgAwIBAgIUXwMCEiQD9wdyxF030YLC8conbUYwDQYJKoZIhvcNAQEL
BQAwOjESMBAGA1UEAwwJbG9jYWxob3N0MSQwIgYDVQQKDBtoYXZSZW1vdGUgSW50
ZWdyYXRpb24gVGVzdHMwHhcNMjYwODI5MTA1MjM1WhcNMzYwODI2MTA1MjM1WjA6
MRIwEAYDVQQDDAlsb2NhbGhvc3QxJDAiBgNVBAoMG2hhdlJlbW90ZSBJbnRlZ3Jh
dGlvbiBUZXN0czCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALDGMdSU
AQo+YMn0oMnuxv9M1S4+fg8NhVb84kMXfQk+Qe4wiXkdNM52bwnJ/cmoHeZnj+VH
R+f1g9B4BKvd8dtBlobYpzAsP1qzIZocLJ/Oxirg4iV4DEM57eLkTDM3AnJksim2
MctNVuv/xhfj4BrGW4p4QX5/T1ic/wbGdDk84KkikXKGhVicXTOQgEazqQqnObL1
vOpxOc9BEtiPXlR2NE0XLH84vHdNDak8rSZKBV+jvtMoiR5o6Ix+qzLebNajGKFU
dQkzPJsUeLojD8oFNCfUulXXAWHMWIdiD1/G2+zSc554xZr0sJgvLmefmAqmr/ur
Drhyw803OCYR06MCAwEAAaNvMG0wHQYDVR0OBBYEFBvJXw6aI9Kqr6SpL+ISqZO2
x6nQMB8GA1UdIwQYMBaAFBvJXw6aI9Kqr6SpL+ISqZO2x6nQMA8GA1UdEwEB/wQF
MAMBAf8wGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMA0GCSqGSIb3DQEBCwUA
A4IBAQAimpg8Kv4c9ii3j9XapKncFoLy2P7pA5Wlw88vnPth6zNL+saxl/iyhW50
7/39dfC7tF3vCp7KVtzlGxfykj62G2EaBjuCg5mWL2cQ110kaZ7E01AsopLJhH1V
gm+Zha33BJjUon5KRmZsUSZ5Wq2j3NMzErWVBfjnDAhM6awdMgRB+bYRbs//4cZi
ah3Ytg233Syrh8J+ePFbzEZbQFBBga2ctCmZmeqwmoSwCMLum0Qln+TxtjRWPRmE
zkkQUbNj2EZSL+inns4Oq9cdiNGC3+w15PVOODOn9TgfVZLg/8sPdw3ZgNORyRxt
hhrQXPE9yQ/oRc8urL8b/GPLBjSX
-----END CERTIFICATE-----
)CERT";

  const auto identity = ftp::InspectCertificatePem(certificate);

  REQUIRE(identity);
  CHECK(identity->curlPin ==
        "sha256//L4IHQpsCHemCVp2BuMv+FQSwULTsaGuvyedqYug1Bf4=");
  CHECK(identity->sha256Fingerprint ==
        "SHA256:L4IHQpsCHemCVp2BuMv+FQSwULTsaGuvyedqYug1Bf4");
  REQUIRE_FALSE(identity->subjectPublicKeyInfo.empty());
  CHECK(std::to_integer<unsigned char>(identity->subjectPublicKeyInfo.front()) == 0x30U);
  CHECK(identity->genuinelySelfSigned);
  CHECK(identity->currentlyTimeValid);
  CHECK(identity->serverAuthenticationValidExceptTrustAnchor);

  auto forgedCertificate = std::string{certificate};

  const auto signatureTail = forgedCertificate.rfind("BjSX");

  REQUIRE(signatureTail != std::string::npos);

  forgedCertificate[signatureTail + 3U] = 'Y';

  const auto forged = ftp::InspectCertificatePem(forgedCertificate);

  REQUIRE(forged);
  CHECK_FALSE(forged->genuinelySelfSigned);
  CHECK(forged->currentlyTimeValid);
  CHECK_FALSE(forged->serverAuthenticationValidExceptTrustAnchor);

  const auto malformed = ftp::InspectCertificatePem(
      "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n");

  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().code == RemoteErrorCode::ParseError);
}

TEST_CASE("FTPS certificate inspection rejects expired and client-only self-signed leaves")
{
  constexpr std::string_view clientOnly = R"CERT(-----BEGIN CERTIFICATE-----
MIIB5zCCAY2gAwIBAgIUfhXumDXZzyZxrUvdxrzHGBl63fkwCgYIKoZIzj0EAwIw
NTESMBAGA1UEAwwJbG9jYWxob3N0MR8wHQYDVQQKDBZoYXZSZW1vdGUgdGVzdCBm
aXh0dXJlMB4XDTI2MDkxMjIxMTkxOVoXDTM2MDkwOTIxMTkxOVowNTESMBAGA1UE
AwwJbG9jYWxob3N0MR8wHQYDVQQKDBZoYXZSZW1vdGUgdGVzdCBmaXh0dXJlMFkw
EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEMO7TBeMOJ1OdX1nbwJwB3GVSd3VHXdr/
IhZnLQy1hxx/BjslIMenMnTQ0bQXEB8sO6wPZxS+O47AYZQ6yP8drqN7MHkwHQYD
VR0OBBYEFLCdjBFnGHRUSodvRffhJfqpKXjCMB8GA1UdIwQYMBaAFLCdjBFnGHRU
SodvRffhJfqpKXjCMAwGA1UdEwEB/wQCMAAwEwYDVR0lBAwwCgYIKwYBBQUHAwIw
FAYDVR0RBA0wC4IJbG9jYWxob3N0MAoGCCqGSM49BAMCA0gAMEUCIHiC1+P9G+Hd
b05KiS0QyKqhUNHhH8aJ33MSRM7wlBGkAiEAjhI43hqSXwjge4Vv61qqG+lI8obi
dV4nSQ5vq1+wCCo=
-----END CERTIFICATE-----
)CERT";

  const auto clientIdentity = ftp::InspectCertificatePem(clientOnly);

  REQUIRE(clientIdentity);
  CHECK(clientIdentity->genuinelySelfSigned);
  CHECK(clientIdentity->currentlyTimeValid);
  CHECK_FALSE(clientIdentity->serverAuthenticationValidExceptTrustAnchor);

  constexpr std::string_view expired = R"CERT(-----BEGIN CERTIFICATE-----
MIIB5jCCAY2gAwIBAgIUX+AmftC+J5y0OivIhn7jdzLzMM0wCgYIKoZIzj0EAwIw
NTESMBAGA1UEAwwJbG9jYWxob3N0MR8wHQYDVQQKDBZoYXZSZW1vdGUgdGVzdCBm
aXh0dXJlMB4XDTI2MDkxMjIxMTkxOVoXDTI2MDkxMTIxMTkxOVowNTESMBAGA1UE
AwwJbG9jYWxob3N0MR8wHQYDVQQKDBZoYXZSZW1vdGUgdGVzdCBmaXh0dXJlMFkw
EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEMO7TBeMOJ1OdX1nbwJwB3GVSd3VHXdr/
IhZnLQy1hxx/BjslIMenMnTQ0bQXEB8sO6wPZxS+O47AYZQ6yP8drqN7MHkwHQYD
VR0OBBYEFLCdjBFnGHRUSodvRffhJfqpKXjCMB8GA1UdIwQYMBaAFLCdjBFnGHRU
SodvRffhJfqpKXjCMAwGA1UdEwEB/wQCMAAwEwYDVR0lBAwwCgYIKwYBBQUHAwEw
FAYDVR0RBA0wC4IJbG9jYWxob3N0MAoGCCqGSM49BAMCA0cAMEQCIHxo3FlECoqj
V0WTtmO7o9oQ8onb1jbgxeLQG2/vhL2xAiAHgw/Dj+apd70Es45vwUS0rLn1joIT
h1Of6y4CuJPQaw==
-----END CERTIFICATE-----
)CERT";

  const auto expiredIdentity = ftp::InspectCertificatePem(expired);

  REQUIRE(expiredIdentity);
  CHECK(expiredIdentity->genuinelySelfSigned);
  CHECK_FALSE(expiredIdentity->currentlyTimeValid);
  CHECK_FALSE(expiredIdentity->serverAuthenticationValidExceptTrustAnchor);
}
