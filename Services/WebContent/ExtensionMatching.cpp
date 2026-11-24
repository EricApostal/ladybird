/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <WebContent/ExtensionMatching.h>

namespace WebContent {

bool glob_matches(StringView pattern, StringView text, bool question_mark_matches_one, bool ignore_case)
{
    auto equal = [&](char a, char b) {
        if (ignore_case)
            return to_ascii_lowercase(static_cast<u8>(a)) == to_ascii_lowercase(static_cast<u8>(b));
        return a == b;
    };

    constexpr auto no_star = NumericLimits<size_t>::max();
    size_t pattern_index = 0;
    size_t text_index = 0;
    size_t star_index = no_star;
    size_t star_text_index = 0;

    while (text_index < text.length()) {
        if (pattern_index < pattern.length() && pattern[pattern_index] == '*') {
            star_index = pattern_index++;
            star_text_index = text_index;
        } else if (pattern_index < pattern.length()
            && ((question_mark_matches_one && pattern[pattern_index] == '?') || equal(pattern[pattern_index], text[text_index]))) {
            ++pattern_index;
            ++text_index;
        } else if (star_index != no_star) {
            // Let the last `*` swallow one more character and try again.
            pattern_index = star_index + 1;
            text_index = ++star_text_index;
        } else {
            return false;
        }
    }

    while (pattern_index < pattern.length() && pattern[pattern_index] == '*')
        ++pattern_index;
    return pattern_index == pattern.length();
}

static bool parse_port(StringView text, Optional<u16>& port)
{
    if (text == "*"sv) {
        port = {};
        return true;
    }
    auto value = text.to_number<u16>();
    if (!value.has_value())
        return false;
    port = *value;
    return true;
}

Optional<MatchPattern> MatchPattern::parse(StringView source)
{
    MatchPattern pattern;

    if (source == "<all_urls>"sv) {
        pattern.m_all_urls = true;
        pattern.m_path = ByteString { "*"sv };
        return pattern;
    }

    auto scheme_end = source.find("://"sv);
    if (!scheme_end.has_value() || *scheme_end == 0)
        return {};

    auto scheme = source.substring_view(0, *scheme_end);
    if (scheme == "*"sv)
        pattern.m_any_scheme = true;
    else if (scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "ftp"sv, "ftps"sv, "file"sv))
        pattern.m_scheme = ByteString { scheme };
    else
        return {};

    auto rest = source.substring_view(*scheme_end + 3);
    auto path_start = rest.find('/');
    if (!path_start.has_value())
        return {};

    auto authority = rest.substring_view(0, *path_start);
    pattern.m_path = ByteString { rest.substring_view(*path_start) };

    if (scheme == "file"sv) {
        // file: patterns have no host, though Firefox accepts a `*` there, as in `file://*/*`.
        if (!authority.is_empty() && authority != "*"sv)
            return {};
        return pattern;
    }

    if (authority.is_empty())
        return {};

    auto host = authority;
    if (authority.starts_with('[')) {
        auto close = authority.find(']');
        if (!close.has_value())
            return {};
        host = authority.substring_view(0, *close + 1);
        auto after = authority.substring_view(*close + 1);
        if (!after.is_empty()) {
            if (!after.starts_with(':') || !parse_port(after.substring_view(1), pattern.m_port))
                return {};
        }
    } else if (auto colon = authority.find_last(':'); colon.has_value()) {
        host = authority.substring_view(0, *colon);
        if (!parse_port(authority.substring_view(*colon + 1), pattern.m_port))
            return {};
    }

    if (host != "*"sv) {
        if (host.starts_with("*."sv)) {
            pattern.m_subdomains = true;
            host = host.substring_view(2);
        }
        if (host.is_empty() || host.contains('*'))
            return {};
        pattern.m_host = ByteString { host }.to_lowercase();
    }

    return pattern;
}

bool MatchPattern::matches(URL::URL const& url) const
{
    auto scheme = url.scheme();

    if (m_all_urls) {
        return scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "ftp"sv, "ftps"sv, "data"sv, "file"sv);
    }

    if (m_any_scheme) {
        if (!scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv))
            return false;
    } else if (scheme != m_scheme.view()) {
        return false;
    }

    if (scheme != "file"sv) {
        if (m_host.has_value()) {
            auto host = url.serialized_host();
            auto expected = m_host->view();
            if (host != expected) {
                if (!m_subdomains)
                    return false;
                // `*.example.com` must not match `badexample.com`, so require the dot.
                auto is_subdomain = host.length() > expected.length() + 1
                    && host.ends_with(expected)
                    && host[host.length() - expected.length() - 1] == '.';
                if (!is_subdomain)
                    return false;
            }
        }

        if (m_port.has_value() && url.port_or_default() != *m_port)
            return false;
    }

    if (m_path == "/*"sv || m_path == "*"sv)
        return true;

    StringBuilder target;
    target.append(url.serialize_path());
    if (auto query = url.query(); query.has_value()) {
        target.append('?');
        target.append(*query);
    }
    return glob_matches(m_path, target.string_view(), false, false);
}

}
