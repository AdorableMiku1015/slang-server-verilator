#include "lsp/URI.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("URI Decode") {

    URI u("file:///x/%41%42%43/%20y.z");

#ifndef _WIN32
    CHECK(u.getPath() == "/x/ABC/ y.z");
#else
    CHECK(u.getPath() == R"(\x\ABC\ y.z)");
#endif
}

TEST_CASE("URI EmptyInput") {
    URI u("");

    // TODO: Right now it returns "/" as the URI string.
    // It should return ""
    // CHECK(u.str() == "");

#ifdef _WIN32
    CHECK(u.getPath() == "\\");
#else
    CHECK(u.getPath() == "/");
#endif
}

TEST_CASE("URI LongInput") {
    // Long URIs used to kill the server before it could answer: the greedy quantifiers in the
    // RFC 3986 pattern made ctre recurse once per input character, so a deep enough path
    // overflowed the stack while a message was being deserialized. These are the shapes VS Code
    // sends for documents in deeply nested or percent encoded directories.
    const std::string filler(6000, 'd');

    SECTION("Path") {
        const std::string text = "file:///C:/" + filler + "/deep/file.sv";
        URI u(text);

        CHECK(u.str() == text);
#ifdef _WIN32
        CHECK(u.getPath() == "C:\\" + filler + "\\deep\\file.sv");
#else
        CHECK(u.getPath() == "/C:/" + filler + "/deep/file.sv");
#endif
    }

    SECTION("PercentEncoded") {
        // "%E4%B8%AD%E6%96%87" is UTF-8 for 中文, so a non-ascii path inflates the URI length
        const std::string decoded = "\xE4\xB8\xAD\xE6\x96\x87";
        URI u("file:///C:/" + filler + "/%E4%B8%AD%E6%96%87.sv");

        CHECK(u.getPath().ends_with(decoded + ".sv"));
    }

    SECTION("QueryAndFragment") {
        URI u("file:///C:/" + filler + "?q=" + filler + "#" + filler);
        URI withoutQueryOrFragment("file:///C:/" + filler);

        // Query and fragment are split off the path
        CHECK(u.getPath().find('?') == std::string_view::npos);
        CHECK(u.getPath().find('#') == std::string_view::npos);
        CHECK(u.getPath().size() == withoutQueryOrFragment.getPath().size());
    }
}

#ifdef _WIN32

TEST_CASE("URI WindowsDriveLetter") {
    SECTION("HexDecoded") {
        URI u("file:///c:/temp/file.txt");

        CHECK(u.str() == "file:///C:/temp/file.txt");
        CHECK(u.getPath() == "C:\\temp\\file.txt");
    }

    SECTION("HexEncoded") {
        URI u("file:///c%3A/temp/file.txt");

        CHECK(u.str() == "file:///C:/temp/file.txt");
        CHECK(u.getPath() == "C:\\temp\\file.txt");
    }
}

TEST_CASE("URI WindowsUNCPath") {
    SECTION("UNC Basic") {
        URI u("file://server/share/file.txt");

        CHECK(u.str() == "file://server/share/file.txt");
        CHECK(u.getPath() == R"(\\server\share\file.txt)");
    }

    SECTION("UNC FromFile") {
        std::filesystem::path p(R"(\\server\share\file.txt)");
        URI u = URI::fromFile(p);

        CHECK(u.str() == "file://server/share/file.txt");
        CHECK(u.getPath() == R"(\\server\share\file.txt)");
    }
}

#endif
