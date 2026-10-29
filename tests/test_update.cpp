#include <string>

#include "doctest.h"
#include "fit/sha256.hpp"
#include "fit/update.hpp"

using namespace fit;

TEST_CASE("SHA-256 matches the standard test vectors") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 s;
    const std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.update(a.data(), a.size());  // one million 'a'
    CHECK(s.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("versions compare numerically") {
    CHECK(parse_version("v0.2.0").ok);
    CHECK(compare_versions(parse_version("v0.10.0"), parse_version("0.9.9")) > 0);
    CHECK(compare_versions(parse_version("0.2.0 (preview)"), parse_version("v0.2.0")) == 0);
    CHECK(compare_versions(parse_version("0.2.1"), parse_version("0.2")) > 0);
    CHECK_FALSE(parse_version("preview").ok);
}

TEST_CASE("a GitHub release is read, and one without the program or its checksum is refused") {
    const std::string ok = R"({"tag_name":"v0.3.0","name":"FitSuite 0.3.0","body":"Faster rounds.","published_at":"2026-11-01T10:00:00Z",
        "prerelease":false,"assets":[{"name":"FitSuite.zip","browser_download_url":"https://example.com/a.zip","size":1},
        {"name":"FitSuite.exe","browser_download_url":"https://github.com/x/y/releases/download/v0.3.0/FitSuite.exe","size":8000000},
        {"name":"FitSuite.exe.sha256","browser_download_url":"https://github.com/x/y/releases/download/v0.3.0/FitSuite.exe.sha256","size":80}]})";
    std::string err;
    const auto r = parse_github_release(ok, &err);
    REQUIRE(r);
    CHECK(r->version.minor == 3);
    CHECK(r->exe_size == 8000000);
    CHECK(r->notes == "Faster rounds.");
    CHECK(r->sha_url.find("FitSuite.exe.sha256") != std::string::npos);

    const std::string no_sha = R"({"tag_name":"v0.3.0","assets":[{"name":"FitSuite.exe","browser_download_url":"https://x/FitSuite.exe"}]})";
    CHECK_FALSE(parse_github_release(no_sha, &err));
    CHECK(err.find("checksum") != std::string::npos);
    const std::string http = R"({"tag_name":"v0.3.0","assets":[{"name":"FitSuite.exe","browser_download_url":"http://x/FitSuite.exe"},
        {"name":"FitSuite.exe.sha256","browser_download_url":"http://x/s"}]})";
    CHECK_FALSE(parse_github_release(http, &err));  // only https
    CHECK_FALSE(parse_github_release(R"({"message":"Not Found"})", &err));
    CHECK_FALSE(parse_github_release("not json", &err));
}

TEST_CASE("checksum files are read in sha256sum format") {
    const std::string h = "770fe3ae0919ca3c4d412f58c985eb27f5434decad809f7e8206de4e8852eec4";
    CHECK(parse_sha256_text(h + "  FitSuite.exe\n") == h);
    CHECK(parse_sha256_text(h) == h);
    CHECK(parse_sha256_text("770FE3AE0919CA3C4D412F58C985EB27F5434DECAD809F7E8206DE4E8852EEC4\r\n") == h);
    CHECK_FALSE(parse_sha256_text("abc"));
    CHECK_FALSE(parse_sha256_text(h + "ff"));
}
