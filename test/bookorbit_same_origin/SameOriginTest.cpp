#include <gtest/gtest.h>

#include "lib/BookOrbit/SameOrigin.h"

using bookorbit::isSameOrigin;
using bookorbit::originOf;

TEST(OriginOf, ExtractsSchemeHostAndExplicitPort) {
  EXPECT_EQ(originOf("https://books.example.com:8443/api/v1/x"), "https://books.example.com:8443");
}

TEST(OriginOf, FillsInTheDefaultPortPerScheme) {
  EXPECT_EQ(originOf("https://books.example.com/api/v1"), "https://books.example.com:443");
  EXPECT_EQ(originOf("http://books.example.com/api/v1"), "http://books.example.com:80");
}

TEST(OriginOf, LowercasesSchemeAndHost) {
  EXPECT_EQ(originOf("HTTPS://Books.Example.COM/x"), "https://books.example.com:443");
}

TEST(OriginOf, HandlesAUrlWithNoPath) {
  EXPECT_EQ(originOf("https://books.example.com"), "https://books.example.com:443");
}

TEST(OriginOf, RejectsMalformedUrls) {
  EXPECT_EQ(originOf(""), "");
  EXPECT_EQ(originOf("books.example.com/x"), "");
  EXPECT_EQ(originOf("ftp://books.example.com/x"), "");
}

TEST(IsSameOrigin, AcceptsADifferentPathOnTheSameHost) {
  EXPECT_TRUE(isSameOrigin("https://books.example.com/api/v1/a", "https://books.example.com/files/b"));
}

TEST(IsSameOrigin, AcceptsAnExplicitDefaultPort) {
  EXPECT_TRUE(isSameOrigin("https://books.example.com/a", "https://books.example.com:443/b"));
}

TEST(IsSameOrigin, RejectsADifferentHost) {
  EXPECT_FALSE(isSameOrigin("https://books.example.com/a", "https://cdn.example.com/b"));
}

TEST(IsSameOrigin, RejectsADifferentPort) {
  EXPECT_FALSE(isSameOrigin("https://books.example.com/a", "https://books.example.com:8443/b"));
}

// A redirect that drops TLS would send the auth headers in clear. Refused.
TEST(IsSameOrigin, RejectsASchemeDowngrade) {
  EXPECT_FALSE(isSameOrigin("https://books.example.com/a", "http://books.example.com/b"));
}

TEST(IsSameOrigin, RejectsMalformedTargets) {
  EXPECT_FALSE(isSameOrigin("https://books.example.com/a", "/files/b"));
  EXPECT_FALSE(isSameOrigin("https://books.example.com/a", ""));
}
