#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace popcorn::periph::stm32
{
/** @brief Round constants of SHA-384, SHA-512 and their truncations, FIPS 180-4 4.2.3. */
inline constexpr std::array<std::uint64_t, 80> sha512_k{
    0x428A'2F98'D728'AE22, 0x7137'4491'23EF'65CD, 0xB5C0'FBCF'EC4D'3B2F, 0xE9B5'DBA5'8189'DBBC,
    0x3956'C25B'F348'B538, 0x59F1'11F1'B605'D019, 0x923F'82A4'AF19'4F9B, 0xAB1C'5ED5'DA6D'8118,
    0xD807'AA98'A303'0242, 0x1283'5B01'4570'6FBE, 0x2431'85BE'4EE4'B28C, 0x550C'7DC3'D5FF'B4E2,
    0x72BE'5D74'F27B'896F, 0x80DE'B1FE'3B16'96B1, 0x9BDC'06A7'25C7'1235, 0xC19B'F174'CF69'2694,
    0xE49B'69C1'9EF1'4AD2, 0xEFBE'4786'384F'25E3, 0x0FC1'9DC6'8B8C'D5B5, 0x240C'A1CC'77AC'9C65,
    0x2DE9'2C6F'592B'0275, 0x4A74'84AA'6EA6'E483, 0x5CB0'A9DC'BD41'FBD4, 0x76F9'88DA'8311'53B5,
    0x983E'5152'EE66'DFAB, 0xA831'C66D'2DB4'3210, 0xB003'27C8'98FB'213F, 0xBF59'7FC7'BEEF'0EE4,
    0xC6E0'0BF3'3DA8'8FC2, 0xD5A7'9147'930A'A725, 0x06CA'6351'E003'826F, 0x1429'2967'0A0E'6E70,
    0x27B7'0A85'46D2'2FFC, 0x2E1B'2138'5C26'C926, 0x4D2C'6DFC'5AC4'2AED, 0x5338'0D13'9D95'B3DF,
    0x650A'7354'8BAF'63DE, 0x766A'0ABB'3C77'B2A8, 0x81C2'C92E'47ED'AEE6, 0x9272'2C85'1482'353B,
    0xA2BF'E8A1'4CF1'0364, 0xA81A'664B'BC42'3001, 0xC24B'8B70'D0F8'9791, 0xC76C'51A3'0654'BE30,
    0xD192'E819'D6EF'5218, 0xD699'0624'5565'A910, 0xF40E'3585'5771'202A, 0x106A'A070'32BB'D1B8,
    0x19A4'C116'B8D2'D0C8, 0x1E37'6C08'5141'AB53, 0x2748'774C'DF8E'EB99, 0x34B0'BCB5'E19B'48A8,
    0x391C'0CB3'C5C9'5A63, 0x4ED8'AA4A'E341'8ACB, 0x5B9C'CA4F'7763'E373, 0x682E'6FF3'D6B2'B8A3,
    0x748F'82EE'5DEF'B2FC, 0x78A5'636F'4317'2F60, 0x84C8'7814'A1F0'AB72, 0x8CC7'0208'1A64'39EC,
    0x90BE'FFFA'2363'1E28, 0xA450'6CEB'DE82'BDE9, 0xBEF9'A3F7'B2C6'7915, 0xC671'78F2'E372'532B,
    0xCA27'3ECE'EA26'619C, 0xD186'B8C7'21C0'C207, 0xEADA'7DD6'CDE0'EB1E, 0xF57D'4F7F'EE6E'D178,
    0x06F0'67AA'7217'6FBA, 0x0A63'7DC5'A2C8'98A6, 0x113F'9804'BEF9'0DAE, 0x1B71'0B35'131C'471B,
    0x28DB'77F5'2304'7D84, 0x32CA'AB7B'40C7'2493, 0x3C9E'BE0A'15C9'BEBC, 0x431D'67C4'9C10'0D4C,
    0x4CC5'D4BE'CB3E'42B6, 0x597F'299C'FC65'7E2A, 0x5FCB'6FAB'3AD6'FAEC, 0x6C44'198C'4A47'5817,
};

/**
 * @brief Initial chaining values of SHA-384, SHA-512, SHA-512/224 and SHA-512/256, in that order,
 *        FIPS 180-4 5.3.4 and 5.3.6.
 */
inline constexpr std::array<std::array<std::uint64_t, 8>, 4> sha512_initial{
    {
     {
            0xCBBB'9D5D'C105'9ED8,
            0x629A'292A'367C'D507,
            0x9159'015A'3070'DD17,
            0x152F'ECD8'F70E'5939,
            0x6733'2667'FFC0'0B31,
            0x8EB4'4A87'6858'1511,
            0xDB0C'2E0D'64F9'8FA7,
            0x47B5'481D'BEFA'4FA4,
        }, {
            0x6A09'E667'F3BC'C908,
            0xBB67'AE85'84CA'A73B,
            0x3C6E'F372'FE94'F82B,
            0xA54F'F53A'5F1D'36F1,
            0x510E'527F'ADE6'82D1,
            0x9B05'688C'2B3E'6C1F,
            0x1F83'D9AB'FB41'BD6B,
            0x5BE0'CD19'137E'2179,
        }, {
            0x8C3D'37C8'1954'4DA2,
            0x73E1'9966'89DC'D4D6,
            0x1DFA'B7AE'32FF'9C82,
            0x679D'D514'582F'9FCF,
            0x0F6D'2B69'7BD4'4DA8,
            0x77E3'6F73'04C4'8942,
            0x3F9D'85A8'6A1D'36C8,
            0x1112'E6AD'91D6'92A1,
        }, {
            0x2231'2194'FC2B'F72C,
            0x9F55'5FA3'C84C'64C2,
            0x2393'B86B'6F53'B151,
            0x9638'7719'5940'EABD,
            0x9628'3EE2'A88E'FFE3,
            0xBE5E'1E25'5386'3992,
            0x2B01'99FC'2C85'B8AA,
            0x0EB7'2DDC'81C5'2CA2,
        }, }
};

/**
 * @brief Compresses one 1024-bit block into a SHA-512 chaining value, FIPS 180-4 6.4.2.
 * @param h Chaining value, eight 64-bit words.
 * @param m Message block as 32 big-endian 32-bit words, the high word of each 64-bit word first.
 */
constexpr void
    sha512_compress(std::span<std::uint64_t, 8> h, std::span<std::uint32_t const, 32> m) noexcept
{
  std::array<std::uint64_t, 80> w{};
  for(std::size_t t = 0; t < 16; ++t)
  {
    w[t] = std::uint64_t{m[2 * t]} << 32 | m[2 * t + 1];
  }
  for(std::size_t t = 16; t < 80; ++t)
  {
    auto const s0 = std::rotr(w[t - 15], 1) ^ std::rotr(w[t - 15], 8) ^ (w[t - 15] >> 7);
    auto const s1 = std::rotr(w[t - 2], 19) ^ std::rotr(w[t - 2], 61) ^ (w[t - 2] >> 6);
    w[t]          = w[t - 16] + s0 + w[t - 7] + s1;
  }
  std::array<std::uint64_t, 8> v{h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]};
  for(std::size_t t = 0; t < 80; ++t)
  {
    auto const& [a, b, c, d, e, f, g, x] = v;
    auto const s1                        = std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41);
    auto const ch                        = (e & f) ^ (~e & g);
    auto const temp1                     = x + s1 + ch + sha512_k[t] + w[t];
    auto const s0                        = std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39);
    auto const maj                       = (a & b) ^ (a & c) ^ (b & c);
    v                                    = {temp1 + s0 + maj, a, b, c, d + temp1, e, f, g};
  }
  for(std::size_t i = 0; i < h.size(); ++i)
  {
    h[i] += v[i];
  }
}
}
