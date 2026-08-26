#include "TestSupport.h"

#include "ForgeConductor/Contracts/IUnicodeCanonicalizer.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h"

#include <compare>
#include <string>
#include <type_traits>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::WindowsUnicodeCanonicalizer;

static_assert(std::is_final_v<Contracts::NfcUtf8Key>);
static_assert(std::is_final_v<WindowsUnicodeCanonicalizer>);
static_assert(std::is_base_of_v<
              Contracts::IUnicodeCanonicalizer,
              WindowsUnicodeCanonicalizer>);

void testCanonicalEquivalenceAndTypedOrdering()
{
    WindowsUnicodeCanonicalizer canonicalizer;
    const auto composed = take(canonicalizer.nfcKey("Caf\xC3\xA9"));
    const auto decomposed = take(canonicalizer.nfcKey("Cafe\xCC\x81"));

    require(composed == decomposed,
            "canonically equivalent UTF-8 strings produced different NFC keys");
    require(composed.value() == "Caf\xC3\xA9",
            "the composed NFC key did not match the expected UTF-8 bytes");

    const auto bmp = take(canonicalizer.nfcKey("\xEE\x80\x80")); // U+E000
    const auto supplementary = take(
        canonicalizer.nfcKey("\xF0\x90\x80\x80")); // U+10000
    require((bmp <=> supplementary) == std::strong_ordering::less,
            "NFC UTF-8 keys did not preserve Unicode scalar order");
}

void testNfcWithoutCompatibilityFolding()
{
    WindowsUnicodeCanonicalizer canonicalizer;
    const auto hangul = take(canonicalizer.nfcKey(
        "\xE1\x84\x80\xE1\x85\xA1")); // U+1100 U+1161
    require(hangul.value() == "\xEA\xB0\x80", // U+AC00
            "NFC did not compose the canonical Hangul sequence");

    const auto ligature = take(canonicalizer.nfcKey("\xEF\xAC\x81")); // U+FB01
    require(ligature.value() == "\xEF\xAC\x81",
            "NFC incorrectly applied compatibility folding");

    const std::string embeddedNull{"a\0b", 3U};
    require(take(canonicalizer.nfcKey(embeddedNull)).value() == embeddedNull,
            "explicit-length canonicalization truncated an embedded U+0000");
}

void testInvalidAndOversizedInputFailsTyped()
{
    WindowsUnicodeCanonicalizer canonicalizer;
    const std::string malformed{"\xC3\x28", 2U};
    requireError(
        canonicalizer.nfcKey(malformed),
        Domain::ErrorCodes::InvalidRequest,
        "the canonicalizer accepted malformed UTF-8");

    const std::string oversized(
        Contracts::IUnicodeCanonicalizer::MaximumInputBytes + 1U,
        'a');
    requireError(
        canonicalizer.nfcKey(oversized),
        Domain::ErrorCodes::PayloadTooLarge,
        "the canonicalizer accepted input beyond its public allocation bound");

    requireError(
        Contracts::NfcUtf8Key::create(malformed),
        Domain::ErrorCodes::IntegrityFailure,
        "the NFC key factory accepted malformed adapter output");
    const std::string oversizedKey(
        Contracts::IUnicodeCanonicalizer::MaximumKeyBytes + 1U,
        'a');
    requireError(
        Contracts::NfcUtf8Key::create(oversizedKey),
        Domain::ErrorCodes::IntegrityFailure,
        "the NFC key factory accepted unbounded adapter output");
}

} // namespace

void registerUnicodeCanonicalizerWindowsTests(TestRegistry& tests)
{
    addTest(
        tests,
        "foundation.unicode_canonical_equivalence_and_typed_ordering",
        testCanonicalEquivalenceAndTypedOrdering);
    addTest(
        tests,
        "foundation.unicode_nfc_without_compatibility_folding",
        testNfcWithoutCompatibilityFolding);
    addTest(
        tests,
        "foundation.unicode_invalid_and_oversized_input_fails_typed",
        testInvalidAndOversizedInputFailsTyped);
}

} // namespace ForgeConductor::Tests
