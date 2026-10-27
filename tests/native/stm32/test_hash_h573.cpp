#include "factories.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::h573;
using popcorn::testing::rcc_reset_pulse;
using popcorn::testing::saved_integer;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

using bytes = std::vector<std::uint8_t>;

constexpr std::string_view firmware = "h573_rcc_ll";

constexpr std::uint32_t hash_base = 0x420C'0400;
constexpr std::uint32_t cr        = 0x00;
constexpr std::uint32_t din       = 0x04;
constexpr std::uint32_t str       = 0x08;
constexpr std::uint32_t hra0      = 0x0C;
constexpr std::uint32_t imr       = 0x20;
constexpr std::uint32_t sr        = 0x24;
constexpr std::uint32_t csr0      = 0xF8;
constexpr std::uint32_t hr0       = 0x310;
constexpr std::uint32_t cr_init   = 1u << 2;
constexpr std::uint32_t cr_dmae   = 1u << 3;
constexpr std::uint32_t cr_mode   = 1u << 6;
constexpr std::uint32_t cr_mdmat  = 1u << 13;
constexpr std::uint32_t cr_lkey   = 1u << 16;
constexpr std::uint32_t str_dcal  = 1u << 8;
constexpr std::uint32_t sr_dinis  = 1u << 0;
constexpr std::uint32_t sr_dcis   = 1u << 1;
constexpr std::uint32_t sr_busy   = 1u << 3;
constexpr std::uint32_t imr_dcie  = 1u << 1;
constexpr std::uint32_t cr_saved = cr_dmae | (3u << 4) | cr_mode | cr_mdmat | cr_lkey | (15u << 17);
constexpr std::uint32_t hr_count = 42;
constexpr std::uint64_t ms       = 1'000'000'000;

constexpr std::uint32_t sha1       = 0;
constexpr std::uint32_t sha224     = 2;
constexpr std::uint32_t sha256     = 3;
constexpr std::uint32_t sha3_224   = 4;
constexpr std::uint32_t sha3_256   = 5;
constexpr std::uint32_t sha3_384   = 6;
constexpr std::uint32_t sha3_512   = 7;
constexpr std::uint32_t shake128   = 8;
constexpr std::uint32_t shake256   = 9;
constexpr std::uint32_t sha384     = 12;
constexpr std::uint32_t sha512_224 = 13;
constexpr std::uint32_t sha512_256 = 14;
constexpr std::uint32_t sha512     = 15;

constexpr auto digest_bytes(std::uint32_t algo) -> std::size_t
{
  switch(algo)
  {
    case sha1:
      return 20;
    case sha224:
    case sha3_224:
    case sha512_224:
      return 28;
    case sha3_384:
    case sha384:
      return 48;
    case sha3_512:
    case sha512:
      return 64;
    case shake128:
      return 168;
    case shake256:
      return 136;
    default:
      return 32;
  }
}

constexpr auto block_bytes(std::uint32_t algo) -> std::size_t
{
  switch(algo)
  {
    case sha3_224:
      return 144;
    case sha3_256:
    case shake256:
      return 136;
    case sha3_384:
      return 104;
    case sha3_512:
      return 72;
    case shake128:
      return 168;
    case sha384:
    case sha512_224:
    case sha512_256:
    case sha512:
      return 128;
    default:
      return 64;
  }
}

constexpr auto keccak_algo(std::uint32_t algo) -> bool
{
  return algo >= sha3_224 and algo <= shake256;
}

constexpr auto cycles_of(std::uint32_t algo) -> std::uint64_t
{
  switch(algo)
  {
    case sha1:
      return 82;
    case sha3_224:
      return 60;
    case sha3_256:
      return 58;
    case sha3_384:
      return 50;
    case sha3_512:
      return 42;
    case shake256:
      return 58;
    case sha384:
    case sha512_224:
    case sha512_256:
    case sha512:
      return 98;
    default:
      return 66;
  }
}

auto pattern(std::size_t size, std::uint32_t seed = 0) -> bytes
{
  bytes out(size);
  for(std::size_t index = 0; index < size; ++index)
  {
    out[index] = static_cast<std::uint8_t>(index * 37 + 11 + seed * 13);
  }
  return out;
}

auto text(std::string_view message) -> bytes
{
  return {message.begin(), message.end()};
}

auto unhex(std::string_view hex) -> bytes
{
  bytes out;
  for(std::size_t index = 0; index + 1 < hex.size(); index += 2)
  {
    auto const nibble = [](char c) {
      return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    out.push_back(static_cast<std::uint8_t>(nibble(hex[index]) << 4 | nibble(hex[index + 1])));
  }
  return out;
}

auto field(std::uint32_t word, unsigned lsb, unsigned width) -> std::uint32_t
{
  return (word >> lsb) & ((1u << width) - 1u);
}

struct digest_case
{
  std::uint32_t    algo;
  std::string_view message;
  std::size_t      size;
  std::string_view digest;

  [[nodiscard]] auto input() const -> bytes
  {
    return message.empty() ? pattern(size) : text(message);
  }
};

struct shake_case
{
  std::uint32_t    algo;
  std::string_view message;
  std::size_t      size;
  std::size_t      length;
  std::string_view digest;
};

struct hmac_case
{
  std::uint32_t    algo;
  std::size_t      key;
  std::size_t      data;
  bool             long_key;
  std::string_view digest;
};

constexpr std::array<digest_case, 52> digests{
    {
     {0, "abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d"},
     {2, "abc", 3, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
     {3, "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
     {12, "abc", 3,
         "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca1"
         "34c825a7"},
     {12,
         {},
         0,
         "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f1"
         "4898b95b"},
     {13, "abc", 3, "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa"},
     {13, {}, 0, "6ed0dd02806fa89e25de060c19d3ac86cabb87d6a0ddd05c333b84f4"},
     {14, "abc", 3, "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23"},
     {14, {}, 0, "c672b8d1ef56ed28ab87c3622c5114069bdd3ad7b8f9737498d0c01ecef0967a"},
     {15, "abc", 3,
         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23"
         "a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
     {15,
         {},
         0,
         "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2"
         "877eec2f63b931bd47417a81a538327af927da3e"},
     {3,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
     {12,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112,
         "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa"
         "91746039"},
     {13,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112, "23fec5bb94d60b23308192640b0c453335d664734fe40e7268674af9"},
     {14,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112, "3928e184fb8690f840da3988121d31be65cb9d3ef83ee6146feac861e19b563a"},
     {15,
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
         "mnopqrsmnopqrstnopqrstu",
         112,
         "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99de"
         "c4b5433ac7d329eeb6dd26545e96e55b874be909"},
     {12,
         {},
         111,
         "d6517d210fc664af0e137a4ca19d7a299465109f40cc0fa9ac7f9d4ac9c77e6687f3cb060890ef7e96f9f525"
         "2dc528ab"},
     {12,
         {},
         112,
         "76078618f7c580d9ca1f9990c2adb38a899f7feda86153dda507b72f1bbafb046ab835aa8d325a89a67e3deb"
         "81ad1ad5"},
     {12,
         {},
         128,
         "13274b7eaabb6a3203608240e792e825ddb816853a66fde047b368cc7638d61b9f215323c4160677301ae4d1"
         "78cd8579"},
     {12,
         {},
         300,
         "981948ac75788a432936364b4d2c89bb8a5e0f0637e46eb05a845b3a4cc2ac1bbbe829cccf3ba46bd33a15d0"
         "c4a3cdf7"},
     {13, {}, 111, "a58fca1508a10042d2726853fd87baedc738d8c9353f05c18e3d917f"},
     {13, {}, 112, "910acbee9104e54d812d15d96b7c0e6ece09a62b144c3ffec072fdc9"},
     {13, {}, 128, "ed05e62e49b2d9345131438d9da5b9e764910ba482f503132abe92d9"},
     {13, {}, 300, "197368fb2b7a9ee40696e4f82c69ba117ad2b2bb6f9de87eaaceb3ad"},
     {14, {}, 111, "84d71ab5b507d9440277cc05cdde25e57e396e3a880ee24b0009fade06bc4cd7"},
     {14, {}, 112, "ff0114959575adb2a8a59ddecba0e51855b385eb4a24017c923a0998062698f2"},
     {14, {}, 128, "aa399434cc7ac88f2a667659396d5e9659ccbdc53d8f0d0c9ccd78dbfd970921"},
     {14, {}, 300, "8524b162773b4a5458e47e73ac4fc4acaa85c0a410ced934b33a19f15c5ae79d"},
     {15,
         {},
         111,
         "4d1db900250c96436052fbca79c13acbf378aad9c35b87d94c3803264df61fd22cbd327c8938d024db372abf"
         "4208934ee09367d571d6c670bf74ee07b83e7506"},
     {15,
         {},
         112,
         "dfb715ca3478a894302ace39c42d1d6646e1044f2247a6274d8b42d155d2fdbe7017195e85cfba96bedc51f8"
         "4c44638978a540039ff09c64cef6c0c5ccc8f7b6"},
     {15,
         {},
         128,
         "0b4815d35f9d07b1a30de2790e1be2a720234295cd7b4d9e9af51719ff90019f1fe6d4e402a7dcc417708502"
         "3dc460ab743dad9b2c1dda42662bda5d3b2e155b"},
     {15,
         {},
         300,
         "ddbe2ce66a8112ce9a18335aca0c7bbf14bbc8102281e28fdc2d7d9b8d99957d14182c566d3faeac4f40c6ca"
         "e9b5c87d8e4d4d12dac695a79641b952955f1dee"},
     {4, "abc", 3, "e642824c3f8cf24ad09234ee7d3c766fc9a3a5168d0c94ad73b46fdf"},
     {4, {}, 0, "6b4e03423667dbb73b6e15454f0eb1abd4597f9a1b078e3f5b5a6bc7"},
     {4, {}, 143, "6b5a38053122870b68c0892afe9978d0ebc787a47ed36f570e3cb2d7"},
     {4, {}, 144, "7d7a1d3ab15a323d7d4bef30103361d00b995d033a8647fc39521bfd"},
     {4, {}, 145, "35c009f71b2e266863b539f7fb51ae1a565a561b2f82e750c8292b0f"},
     {5, "abc", 3, "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"},
     {5, {}, 0, "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"},
     {5, {}, 135, "3aa81a5b233ce753b2ab56b3c922338134eb11b8dc3d877d0bc8d19751684b76"},
     {5, {}, 136, "9f065722983c1b643b3fabbed6e791f6d74f77e6cf5a2d38c07c124465ed5d9f"},
     {5, {}, 137, "30efe517346c818828634cb8a3eb3538c14bc2f280132cf0261ed18a7dd85a8b"},
     {6, "abc", 3,
         "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b298d88cea927ac7f539f1edf2"
         "28376d25"},
     {6,
         {},
         0,
         "0c63a75b845e4f7d01107d852e4c2485c51a50aaaa94fc61995e71bbee983a2ac3713831264adb47fb6bd1e0"
         "58d5f004"},
     {6,
         {},
         103,
         "a8d6dc0c5b130f495d07b62285ee34bab4a94235f95795ef511e92318c22e0944835ce22062ac004f66999cd"
         "340b5dc7"},
     {6,
         {},
         104,
         "86f4554684f5ef3111af066f8a2be32f07fc0fee484320ea1ccb42203da40655ee24caab2f1b60d1c7cccee8"
         "a54fd3f7"},
     {6,
         {},
         105,
         "563fa155b2a1181161208301853f10fa377319147c5bf8ba4d8be2f022bcfd390e94a383383860ec621cba31"
         "e6dd1d24"},
     {7, "abc", 3,
         "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec576"
         "47e3934057340b4cf408d5a56592f8274eec53f0"},
     {7,
         {},
         0,
         "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e940"
         "2c3ac558f500199d95b6d3e301758586281dcd26"},
     {7,
         {},
         71,
         "bf5828621a2ea8d1d96d5a0100852a995705cd5a60bf13e8d5161dd8fc051f0a51c5c0cae86438659f1a0fb3"
         "0efc8a1ed01a837fd93725245f24f3efcc79e1de"},
     {7,
         {},
         72,
         "7163023fb85589362e8f184399accaa14e0d17da87dcc69cece725c9bef23b0e4dfb12ed015213d3e6f5c5cc"
         "98f23c20303dd5a2091df35815f5f242e7a9bfeb"},
     {7,
         {},
         73,
         "c2823e8b428215ecacce41592b976554f6dbdb5aeba524cdcda69d3e86ac791f7750c24720fc0ffec0966909"
         "c519fb53a915f8c9c7b9caba1d2b25d5a3b97cab"},
     }
};

constexpr std::array<shake_case, 4> shakes{
    {
     {8, "abc", 3, 672,
         "5881092dd818bf5cf8a3ddb793fbcba74097d5c526a6d35f97b83351940f2cc844c50af32acd3f2cdd066568"
         "706f509bc1bdde58295dae3f891a9a0fca5783789a41f8611214ce612394df286a62d1a2252aa94db9c53895"
         "6c717dc2bed4f232a0294c857c730aa16067ac1062f1201fb0d377cfb9cde4c63599b27f3462bba4a0ed296c"
         "801f9ff7f57302bb3076ee145f97a32ae68e76ab66c48d51675bd49acc29082f5647584e6aa01b3f5af05780"
         "5f973ff8ecb8b226ac32ada6f01c1fcd4818cb006aa5b4cdb3611eb1e533c8964cacfdf31012cd3fb744d022"
         "25b988b475375faad996eb1b9176ecb0f8b2871723d6dbb804e23357e50732f5cfc904b1319795000d7361d9"
         "e5e1b77b4b8f5774aa1482cfa58f83096bdb2e06a3eed543a38919b57ecbec737f4086be007f8ef80094ceea"
         "8807193d46e9be540b6e99b4c1c71507095028a024e8d39aa8f4c5854cedd50d30a223e7d54e9a24f0a2526b"
         "31002afbd1b4ebea69c8400c3deb4c1c35d6dbb75651b284076f5fde47b4a0586ee173e30bd4d08f2bc59c61"
         "14bdd745d20876bee2bf800bd7d8b5e51536c844c73256f7d1ada1870c7bbaf83af10a6fdd7c029678118154"
         "59cfd02d67b936e975c6007c63ea7ae087f0a6b0a1319668bb61788eaa3d3b78e3f2061adcdead4070859018"
         "03ec6f17f0ec650a292198275211a56bf13f0bf7241268b50d3f1ec807897acba99ced827052ae3921fd210d"
         "66ec315c2d028fcafbc90e0eec294621c59cb50ba173112ddd6330302a926c194898778272282fc8da3fca75"
         "3353cdb7053c02e11ac787fc575314ce37661adaef6b7e4419dcfef952c54eae950530405a7cd2a877ac37a5"
         "e23e34335918771b32c0b809e8ab4efb22bf17b1b8550c77b2135bd1cd85f3909670c7f5c6b4073f5182bb3c"
         "f10d8d9c6f4c72a10d6ac4c0"},
     {8, "", 0, 672,
         "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef263cb1eea988004b93103cfb0a"
         "eefd2a686e01fa4a58e8a3639ca8a1e3f9ae57e235b8cc873c23dc62b8d260169afa2f75ab916a58d9749188"
         "35d25e6a435085b2badfd6dfaac359a5efbb7bcc4b59d538df9a04302e10c8bc1cbf1a0b3a5120ea17cda7cf"
         "ad765f5623474d368ccca8af0007cd9f5e4c849f167a580b14aabdefaee7eef47cb0fca9767be1fda69419df"
         "b927e9df07348b196691abaeb580b32def58538b8d23f87732ea63b02b4fa0f4873360e2841928cd60dd4cee"
         "8cc0d4c922a96188d032675c8ac850933c7aff1533b94c834adbb69c6115bad4692d8619f90b0cdf8a7b9c26"
         "4029ac185b70b83f2801f2f4b3f70c593ea3aeeb613a7f1b1de33fd75081f592305f2e4526edc09631b10958"
         "f464d889f31ba010250fda7f1368ec2967fc84ef2ae9aff268e0b1700affc6820b523a3d917135f2dff2ee06"
         "bfe72b3124721d4a26c04e53a75e30e73a7a9c4a95d91c55d495e9f51dd0b5e9d83c6d5e8ce803aa62b8d654"
         "db53d09b8dcff273cdfeb573fad8bcd45578bec2e770d01efde86e721a3f7c6cce275dabe6e2143f1af18da7"
         "efddc4c7b70b5e345db93cc936bea323491ccb38a388f546a9ff00dd4e1300b9b2153d2041d205b443e41b45"
         "a653f2a5c4492c1add544512dda2529833462b71a41a45be97290b6f4cffda2cf990051634a4b1edf6114fb4"
         "9083c1fa3b302ee097f051266be69dc716fdeef91b0d4ab2de525550bf80dc8a684bc3b5a4d46b7efae7afdc"
         "6292988dc9acae03f8634486c1abe2781aae4c02f3460d2cd4e6a463a2ba9562ee623cf0e9f82ab4d0b5c9d0"
         "40a269366479dff0038abfaf2e0ff21f36968972e3f104ddcbe1eb831a87c213162e29b34adfa564d121e9f6"
         "e7729f4203fc5c6c22fa7a73"},
     {9, "abc", 3, 544,
         "483366601360a8771c6863080cc4114d8db44530f8f1e1ee4f94ea37e78b5739d5a15bef186a5386c75744c0"
         "527e1faa9f8726e462a12a4feb06bd8801e751e41385141204f329979fd3047a13c5657724ada64d2470157b"
         "3cdc288620944d78dbcddbd912993f0913f164fb2ce95131a2d09a3e6d51cbfc622720d7a75c6334e8a2d7ec"
         "71a7cc29cf0ea610eeff1a588290a53000faa79932becec0bd3cd0b33a7e5d397fed1ada9442b99903f4dcfd"
         "8559ed3950faf40fe6f3b5d710ed3b677513771af6bfe11934817e8762d9896ba579d88d84ba7aa3cdc7055f"
         "6796f195bd9ae788f2f5bb96100d6bbaff7fbc6eea24d4449a2477d172a5507dcc931412fc346b1bb39b8783"
         "30e026b12ddf384af3334560ea1d363966caa7d8ddcbec7da52b42215c11d5f8ee57f341e399343ce63a752f"
         "c5edec99124a0eb314403e5f358b8b83d05be2d2970099284b00dcc33d7c753d1f752ab743325bc53d91aa67"
         "1e50f9c3f93abf6e9662f90145c61954f2abbd26edad1553ea3a626f359e8f79ade16384e151755c47e822fc"
         "74c5d7100fd31f667564c6debc7d20d99e109f22abcbb8c86c0c1146333903422252a5d34a896770b6cb99a1"
         "f33935bf87a8e47b0549dc2539de9e6a9d1e596111b660cb6b3e0040b4d4916f886dd0b6f1a702849440b99d"
         "6088e20203aebafa8e9dffa94ed35ef1f41f5fdf549fbcc5a0f68298fa009a54d3a659c3c66542b1e3ecaa0d"
         "63e7bcf08d5381f245b2ad6f3f204b96"},
     {9, "", 0, 544,
         "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762fd75dc4ddd8c0f200cb05019d"
         "67b592f6fc821c49479ab48640292eacb3b7c4be141e96616fb13957692cc7edd0b45ae3dc07223c8e92937b"
         "ef84bc0eab862853349ec75546f58fb7c2775c38462c5010d846c185c15111e595522a6bcd16cf86f3d12210"
         "9e3b1fdd943b6aec468a2d621a7c06c6a957c62b54dafc3be87567d677231395f6147293b68ceab7a9e0c58d"
         "864e8efde4e1b9a46cbe854713672f5caaae314ed9083dab4b099f8e300f01b8650f1f4b1d8fcf3f3cb53fb8"
         "e9eb2ea203bdc970f50ae55428a91f7f53ac266b28419c3778a15fd248d339ede785fb7f5a1aaa96d313eacc"
         "890936c173cdcd0fab882c45755feb3aed96d477ff96390bf9a66d1368b208e21f7c10d04a3dbd4e360633e5"
         "db4b602601c14cea737db3dcf722632cc77851cbdde2aaf0a33a07b373445df490cc8fc1e4160ff118378f11"
         "f0477de055a81a9eda57a4a2cfb0c83929d310912f729ec6cfa36c6ac6a75837143045d791cc85eff5b21932"
         "f23861bcf23a52b5da67eaf7baae0f5fb1369db78f3ac45f8c4ac5671d85735cdddb09d2b1e34a1fc066ff4a"
         "162cb263d6541274ae2fcc865f618abe27c124cd8b074ccd516301b91875824d09958f341ef274bdab0bae31"
         "6339894304e35877b0c28a9b1fd166c796b9cc258a064a8f57e27f2a5b8d548a728c9444ecb879adc19de0c1"
         "b8587de3e73e15d3ce2db7c9fa7b58ff"},
     }
};

constexpr std::array<hmac_case, 13> hmacs{
    {
     {15, 200, 150, true,
         "b268aeeb18961a142921813129f58a6d76752361215c581f123709f33d82be3f1f0053577bbdfefec3d0c251"
         "92ca5854825db440cb44c247949655e7de73216d"},
     {15, 20, 3, false,
         "b71c905d2fee85fe33de568c2cfe5875df7cbb065fed7e2cde16c6c61fc7377ff4aefafe837e8b55652df500"
         "d804a3134aca60957f5467da31fe8d1ea940d7d5"},
     {12, 100, 10, false,
         "23ff73f012eee74226e37bb094311940e2f813ee9362773900dc4d0e0bf0c953e951f4df8cc6b4b3a61a45a3"
         "0a24f51b"},
     {5, 200, 50, true, "15243ebfa61fb8512e0489d74c1d6d516f8643a532e9a6485dc2370b1b8e34e5"},
     {4, 20, 60, false, "6ccd59fadb48ba66bdb2e663b5dcd6f4a4387fb114716355d957ca10"},
     {6, 104, 30, false,
         "bbfceb6c05d7d20640749a2650b35f4d47b280806ebd90de9625d3787ef180f30569a12c1c6fd5e70baf817d"
         "24b8bd69"},
     {7, 72, 200, false,
         "ca79924774e71a0bdbf60933f211215a39a8f0f99d2b16ab507b7db3732bc3e796f462d32e523e8f1feb5ac0"
         "dab651fe02d33f1b62bc66ca2dab778a19f57c81"},
     {14, 128, 40, false, "e1576f99dd659c21decc48e7c6b639c62840555356da7658512da6c560eeefe3"},
     {5, 136, 136, false, "1a397bbeca914d1ddbab55c7e93bfe4891412a4494b2079d07b220842d194e6b"},
     {5, 21, 50, false, "197bdfd77037a6751ae53f3374a69551f33eef3001de1f0c0e1c1b0cc441d7d6"},
     {7, 200, 5, true,
         "5a78715314509b1e34c2a2a22f1406478f97f2e37b4eb203339c8e8749ba85ec6c75e150556ce8096f71d4b2"
         "2f86b7a48648b821eb735b2d5d5302f37c0ce146"},
     {5, 40, 300, false, "c6c70ddb631f807a53b5d248e1c069b5485e51284660194326f2d33947a39ba1"},
     {15, 20, 300, false,
         "c5919ea737cfbb09daaace6a54bac786c800123874531c724562d97ddd19178bbf8d420f3798bd41766b5ab1"
         "f33285e5625aef5205071ffffe55af2502bbd1fe"},
     }
};

constexpr std::string_view fips896 =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopq"
    "rsmnopqrstnopqrstu";

struct context
{
  std::uint32_t              imr;
  std::uint32_t              str;
  std::uint32_t              cr;
  std::vector<std::uint32_t> csr;
};

struct rig
{
  explicit rig(bool strict = true)
  {
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(h573(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
  }

  [[nodiscard]] auto read(std::uint32_t offset) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(hash_base + offset, 4);
    REQUIRE(value.has_value());
    return *value;
  }

  void write(std::uint32_t offset, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(hash_base + offset, 4, value).has_value());
  }

  void begin(std::uint32_t algo, std::uint32_t datatype = 2) const
  {
    write(cr, cr_init | (algo << 17) | (datatype << 4));
  }

  void feed(bytes const& message, std::uint32_t datatype = 2) const
  {
    for(std::size_t at = 0; 4 * at < message.size(); ++at)
    {
      std::uint32_t word = 0;
      for(std::size_t byte = 0; byte < 4; ++byte)
      {
        auto const index = 4 * at + byte;
        auto const value =
            index < message.size() ? message[index] : static_cast<std::uint8_t>(0x5Au ^ index);
        word = (word << 8) | value;
      }
      write(din, unswap(word, datatype));
    }
  }

  static auto unswap(std::uint32_t word, std::uint32_t datatype) -> std::uint32_t
  {
    switch(datatype)
    {
      case 1:
        return std::rotl(word, 16);
      case 2:
        return std::byteswap(word);
      case 3:
      {
        std::uint32_t out = 0;
        for(unsigned bit = 0; bit < 32; ++bit)
        {
          out = (out << 1) | ((word >> bit) & 1u);
        }
        return out;
      }
      default:
        return word;
    }
  }

  void finish(std::size_t size) const
  {
    write(str, static_cast<std::uint32_t>(8 * (size % 4)));
    write(str, str_dcal);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  [[nodiscard]] auto cycle_ps() const -> std::uint64_t
  {
    return 1'000'000'000'000 / f->target->virtual_clock().core_hz();
  }

  void until(std::uint64_t ps) const
  {
    auto const stopped =
        f->run_to_stop({.max_instructions = icount{1'000'000}, .until_time = vtime{ps}});
    REQUIRE(stopped.reason == stop_reason::time);
  }

  void settle() const
  {
    until(now() + ms);
  }

  [[nodiscard]] auto words(std::size_t count) const -> bytes
  {
    bytes out;
    for(std::uint32_t index = 0; index < count / 4; ++index)
    {
      auto const word = read(hr0 + 4 * index);
      for(int shift = 24; shift >= 0; shift -= 8)
      {
        out.push_back(static_cast<std::uint8_t>(word >> shift));
      }
    }
    return out;
  }

  [[nodiscard]] auto digest(std::uint32_t algo) const -> bytes
  {
    REQUIRE((read(sr) & sr_dcis) != 0);
    auto const size = digest_bytes(algo);
    for(std::uint32_t index = size / 4; index < hr_count; ++index)
    {
      CHECK(read(hr0 + 4 * index) == 0);
    }
    for(std::uint32_t index = 0; index < 5; ++index)
    {
      CHECK(read(hra0 + 4 * index) == (keccak_algo(algo) ? 0u : read(hr0 + 4 * index)));
    }
    return words(size);
  }

  [[nodiscard]] auto
      hash(std::uint32_t algo, bytes const& message, std::uint32_t datatype = 2) const -> bytes
  {
    begin(algo, datatype);
    feed(message, datatype);
    finish(message.size());
    settle();
    return digest(algo);
  }

  [[nodiscard]] auto squeeze(std::uint32_t algo, bytes const& message, std::size_t length) const
      -> bytes
  {
    begin(algo);
    feed(message);
    finish(message.size());
    settle();
    bytes      out;
    auto const rate = digest_bytes(algo);
    while(out.size() < length)
    {
      REQUIRE((read(sr) & sr_dcis) != 0);
      auto const part = words(rate);
      out.insert(out.end(), part.begin(), part.end());
      write(str, str_dcal);
      settle();
    }
    out.resize(length);
    return out;
  }

  void hmac_begin(std::uint32_t algo, bool long_key, std::uint32_t datatype = 2) const
  {
    write(cr, cr_init | cr_mode | (long_key ? cr_lkey : 0u) | (algo << 17) | (datatype << 4));
  }

  void phase(bytes const& data, std::uint32_t datatype = 2) const
  {
    feed(data, datatype);
    finish(data.size());
    settle();
  }

  [[nodiscard]] auto hmac(hmac_case const& c, std::uint32_t datatype = 2) const -> bytes
  {
    auto const key  = pattern(c.key, 1);
    auto const data = pattern(c.data, 2);
    hmac_begin(c.algo, c.long_key, datatype);
    phase(key, datatype);
    phase(data, datatype);
    phase(key, datatype);
    return digest(c.algo);
  }

  [[nodiscard]] auto save(std::size_t count) const -> context
  {
    REQUIRE((read(sr) & (sr_busy | sr_dinis)) == sr_dinis);
    context out{read(imr) & 3u, read(str) & 0x1Fu, read(cr) & cr_saved, {}};
    for(std::uint32_t index = 0; index < count; ++index)
    {
      out.csr.push_back(read(csr0 + 4 * index));
    }
    return out;
  }

  void restore(context const& saved) const
  {
    write(imr, saved.imr);
    write(str, saved.str);
    write(cr, saved.cr);
    write(cr, saved.cr | cr_init);
    for(std::uint32_t index = 0; index < saved.csr.size(); ++index)
    {
      write(csr0 + 4 * index, saved.csr[index]);
    }
  }

  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;
};

auto find_hmac(std::uint32_t algo, std::size_t key, std::size_t data) -> hmac_case const&
{
  auto const found = std::ranges::find_if(hmacs, [&](hmac_case const& c) {
    return c.algo == algo and c.key == key and c.data == data;
  });
  REQUIRE(found != hmacs.end());
  return *found;
}

auto find_digest(std::uint32_t algo, std::size_t size, std::string_view message = {})
    -> digest_case const&
{
  auto const found = std::ranges::find_if(digests, [&](digest_case const& c) {
    return c.algo == algo and c.size == size and c.message == message;
  });
  REQUIRE(found != digests.end());
  return *found;
}
}

TEST_CASE("the H5 HASH has ALGO in four bits and HR0 to HR41")
{
  rig const r;
  r.write(cr, 15u << 17);
  CHECK(field(r.read(cr), 17, 4) == 15);
  CHECK(r.read(sr) == 0x0011'0001);
  CHECK(r.read(hr0 + 4 * 41) == 0);
  CHECK(r.logged.empty());
}

TEST_CASE("a family with fewer codes than the ALGO field holds is a config_error")
{
  auto options = popcorn::testing::stm32_options();
  options.models.emplace("HASH", popcorn::periph::stm32::hash_factory());
  CHECK_THROWS_WITH_AS(
      stm32_fixture(h573(), firmware, std::move(options)), doctest::Contains("CR.ALGO"),
      popcorn::config_error);
}

TEST_CASE("FIPS 180-4, FIPS 202 and pattern vectors hash through byte-swapped DIN words")
{
  rig const r;
  for(auto const& c : digests)
  {
    CAPTURE(c.algo);
    CAPTURE(c.size);
    CHECK(r.hash(c.algo, c.input()) == unhex(c.digest));
  }
  CHECK(r.logged.empty());
}

TEST_CASE("the SHA-512 family hashes the 896-bit FIPS message")
{
  rig const r;
  for(auto const algo : {sha256, sha384, sha512_224, sha512_256, sha512})
  {
    CAPTURE(algo);
    CHECK(r.hash(algo, text(fips896)) == unhex(find_digest(algo, 112, fips896).digest));
  }
}

TEST_CASE("every DATATYPE hashes the same message to the same digest as the model maps them")
{
  rig const r;
  for(auto const algo : {sha512, sha3_256})
  {
    auto const expected = r.hash(algo, pattern(300));
    for(std::uint32_t datatype = 0; datatype < 4; ++datatype)
    {
      CAPTURE(algo);
      CAPTURE(datatype);
      CHECK(r.hash(algo, pattern(300), datatype) == expected);
    }
  }
}

TEST_CASE("SHAKE128 and SHAKE256 squeeze 4 rates through repeated DCAL")
{
  rig const r;
  for(auto const& c : shakes)
  {
    CAPTURE(c.algo);
    CAPTURE(c.message);
    auto const message = c.message.empty() ? pattern(c.size) : text(c.message);
    CHECK(r.squeeze(c.algo, message, c.length) == unhex(c.digest));
  }
  CHECK(r.logged.empty());
}

TEST_CASE("a DCAL after a fixed-length digest keeps the digest")
{
  rig const  r;
  auto const first = r.hash(sha3_256, text("abc"));
  r.write(str, str_dcal);
  r.settle();
  CHECK(r.digest(sha3_256) == first);
}

TEST_CASE("a DIN word after a SHAKE digest with no INIT is refused under strict peripherals")
{
  {
    rig const r;
    r.begin(shake128);
    r.feed(text("abc"));
    r.finish(3);
    r.settle();
    CHECK_THROWS_AS(
        static_cast<void>(r.f->target->mem().write(hash_base + din, 4, 0x1234'5678)),
        popcorn::model_error);
  }
  rig const r{false};
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "DIN");
  CHECK(r.words(168) == unhex(shakes[0].digest.substr(0, 336)));
}

TEST_CASE("NBWE after INIT is the block size in words plus 1 in six bits")
{
  rig const r;
  for(auto const algo :
      {sha1, sha224, sha256, sha3_224, sha3_256, sha3_384, sha3_512, shake128, shake256, sha384,
       sha512_224, sha512_256, sha512})
  {
    CAPTURE(algo);
    r.begin(algo);
    CHECK(field(r.read(sr), 16, 6) == block_bytes(algo) / 4 + 1);
  }
  r.begin(sha3_224);
  CHECK(field(r.read(sr), 16, 6) == 37);
}

TEST_CASE("NBWE, NBWP, DINNE and the CR mirrors follow the words of a SHA-3 block")
{
  rig const r;
  r.begin(sha3_224);
  r.feed(pattern(4));
  CHECK(field(r.read(sr), 16, 6) == 36);
  CHECK(field(r.read(sr), 9, 6) == 1);
  CHECK(field(r.read(sr), 15, 1) == 1);
  CHECK(field(r.read(cr), 8, 4) == 1);
  r.feed(pattern(140));
  CHECK(field(r.read(sr), 16, 6) == 1);
  CHECK(field(r.read(sr), 9, 6) == 36);
  CHECK(field(r.read(cr), 8, 4) == (36u & 0xFu));
  r.feed(pattern(4));
  CHECK(field(r.read(sr), 9, 6) == 1);
  CHECK(field(r.read(sr), 16, 6) == 0);
  r.settle();
  CHECK(field(r.read(sr), 16, 6) == 36);
  CHECK((r.read(sr) & sr_dinis) != 0);
}

TEST_CASE("one SHA-3 or SHA-512 block keeps BUSY for its cycles")
{
  rig const r;
  for(auto const algo :
      {sha3_224, sha3_256, sha3_384, sha3_512, shake128, shake256, sha384, sha512})
  {
    CAPTURE(algo);
    r.begin(algo);
    r.feed(pattern(block_bytes(algo)));
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == 0);
    r.feed(pattern(4));
    auto const end = r.now() + cycles_of(algo) * r.cycle_ps();
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_busy);
    r.until(end - r.cycle_ps());
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_busy);
    r.until(end);
    CHECK((r.read(sr) & (sr_dinis | sr_busy)) == sr_dinis);
  }
}

TEST_CASE("a SHAKE squeeze keeps BUSY for one block time")
{
  rig const r;
  for(auto const algo : {shake128, shake256})
  {
    CAPTURE(algo);
    r.begin(algo);
    r.feed(text("abc"));
    r.finish(3);
    r.settle();
    r.write(str, str_dcal);
    auto const end = r.now() + cycles_of(algo) * r.cycle_ps();
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
    r.until(end - r.cycle_ps());
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
    r.until(end);
    CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_dcis);
  }
}

TEST_CASE("NBWP and NBW read 0 after a SHAKE digest as after a SHA-3 digest")
{
  rig const r;
  for(auto const algo : {sha3_256, shake128})
  {
    CAPTURE(algo);
    r.begin(algo);
    r.feed(text("abc"));
    r.finish(3);
    r.settle();
    CHECK(field(r.read(sr), 9, 6) == 0);
    CHECK(field(r.read(cr), 8, 4) == 0);
  }
}

TEST_CASE("a DCAL while BUSY of a SHAKE digest logs once and squeezes at once")
{
  rig const r{false};
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  REQUIRE((r.read(sr) & sr_busy) != 0);
  r.write(str, str_dcal);
  r.settle();
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "STR");
  CHECK(r.words(168) == unhex(shakes[0].digest.substr(336, 336)));
}

TEST_CASE("a second DCAL while BUSY of a SHAKE digest does not log again")
{
  rig const r{false};
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  REQUIRE((r.read(sr) & sr_busy) != 0);
  r.write(str, str_dcal);
  r.settle();
  REQUIRE(r.logged.size() == 1);
  r.write(str, str_dcal);
  REQUIRE((r.read(sr) & sr_busy) != 0);
  r.write(str, str_dcal);
  r.settle();
  CHECK(r.logged.size() == 1);
}

TEST_CASE("a DCAL while BUSY of a SHAKE digest is refused under strict peripherals")
{
  rig const r;
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  REQUIRE((r.read(sr) & sr_busy) != 0);
  CHECK_THROWS_AS(
      static_cast<void>(r.f->target->mem().write(hash_base + str, 4, str_dcal)),
      popcorn::model_error);
  CHECK((r.read(sr) & sr_busy) != 0);
  r.settle();
  CHECK(r.words(168) == unhex(shakes[0].digest.substr(0, 336)));
}

TEST_CASE("NBLW bits 2:0 read zero under SHA-3 and SHAKE and ignore writes")
{
  rig const r;
  r.write(cr, sha3_256 << 17);
  r.write(str, 0x19);
  CHECK(r.read(str) == 0x18);
  r.write(cr, sha256 << 17);
  r.write(str, 0x19);
  CHECK(r.read(str) == 0x19);
  r.write(cr, shake256 << 17);
  CHECK(r.read(str) == 0x18);
  r.begin(sha3_512);
  r.write(din, 0x0063'6261);
  r.write(str, 0x1B);
  r.write(str, str_dcal);
  r.settle();
  CHECK(r.digest(sha3_512) == unhex(find_digest(sha3_512, 3, "abc").digest));
}

TEST_CASE("an NBLW written under SHA2-256 counts whole bytes after INIT selects SHA-3")
{
  rig const r;
  r.write(cr, sha256 << 17);
  r.write(str, 0x19);
  r.begin(sha3_256);
  r.write(din, 0xFF63'6261);
  r.write(str, str_dcal);
  r.settle();
  CHECK(r.read(str) == 0x18);
  CHECK(r.digest(sha3_256) == unhex(find_digest(sha3_256, 3, "abc").digest));
}

TEST_CASE("HRA0 to HRA4 mirror HR0 to HR4 under SHA-2 and read zero under SHA-3")
{
  rig const  r;
  auto const sha2 = r.hash(sha512, text("abc"));
  CHECK(r.read(hra0) == r.read(hr0));
  CHECK(r.read(hra0 + 16) == r.read(hr0 + 16));
  CHECK(r.digest(sha512) == sha2);
  r.begin(sha3_256);
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  CHECK(r.read(hr0) != 0);
  CHECK(r.read(hra0) == 0);
  CHECK(r.read(hra0 + 16) == 0);
}

TEST_CASE("SHAKE fills HR0 to HR41 and SHA3-224 leaves HR7 to HR41 zero")
{
  rig const r;
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  for(std::uint32_t index = 0; index < hr_count; ++index)
  {
    CAPTURE(index);
    CHECK(r.read(hr0 + 4 * index) != 0);
  }
  static_cast<void>(r.hash(sha3_224, text("abc")));
  for(std::uint32_t index = 7; index < hr_count; ++index)
  {
    CHECK(r.read(hr0 + 4 * index) == 0);
  }
}

TEST_CASE("HMAC vectors hash through the three phases")
{
  rig const r;
  for(auto const& c : hmacs)
  {
    CAPTURE(c.algo);
    CAPTURE(c.key);
    CAPTURE(c.data);
    CHECK(r.hmac(c) == unhex(c.digest));
  }
  CHECK(r.hmac(find_hmac(sha3_256, 200, 50), 0) == unhex(find_hmac(sha3_256, 200, 50).digest));
  CHECK(r.logged.empty());
}

TEST_CASE("an HMAC key longer than the block with LKEY 0 logs once and is hashed")
{
  rig const r{false};
  auto      c = find_hmac(sha3_256, 200, 50);
  c.long_key  = false;
  CHECK(r.hmac(c) == unhex(c.digest));
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "DIN");
  CHECK(r.logged[0].message.contains("136 bytes"));
}

TEST_CASE("MODE 1 with SHAKE is refused under strict peripherals and ignored otherwise")
{
  {
    rig const r;
    for(auto const algo : {shake128, shake256})
    {
      CHECK_THROWS_AS(
          static_cast<void>(
              r.f->target->mem().write(hash_base + cr, 4, cr_init | cr_mode | (algo << 17))),
          popcorn::model_error);
      CHECK(r.read(cr) == 0);
    }
  }
  rig const r{false};
  r.write(cr, cr_init | cr_mode | (shake128 << 17) | (2u << 4));
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  CHECK(r.logged.size() == 1);
  CHECK(r.words(168) == unhex(shakes[0].digest.substr(0, 336)));
}

TEST_CASE("the reserved code log and the MODE 1 with SHAKE log are separate")
{
  rig const r{false};
  r.write(cr, cr_init | (10u << 17));
  CHECK(r.logged.size() == 1);
  r.write(cr, cr_init | cr_mode | (shake128 << 17));
  CHECK(r.logged.size() == 2);
  r.write(cr, cr_init | cr_mode | (shake256 << 17));
  r.write(cr, cr_init | (10u << 17));
  CHECK(r.logged.size() == 2);
}

TEST_CASE("the reserved ALGO codes are refused under strict peripherals and run as SHA2-256")
{
  {
    rig const r;
    for(auto const algo : {1u, 10u, 11u})
    {
      CAPTURE(algo);
      CHECK_THROWS_AS(
          static_cast<void>(r.f->target->mem().write(hash_base + cr, 4, cr_init | (algo << 17))),
          popcorn::model_error);
      CHECK(r.read(cr) == 0);
    }
  }
  rig const r{false};
  CHECK(r.hash(1, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
  CHECK(r.hash(10, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "CR");
}

TEST_CASE("a SHA-512 hash saved at a DINIS point resumes after another hash")
{
  auto const message = pattern(300);
  for(std::size_t const words : {0, 33, 65})
  {
    for(auto const count : {91, 103})
    {
      CAPTURE(words);
      CAPTURE(count);
      rig const  r;
      auto const at = static_cast<std::ptrdiff_t>(4 * words);
      r.begin(sha512);
      r.feed(bytes(message.begin(), message.begin() + at));
      r.settle();
      auto const saved = r.save(count);
      CHECK(r.hash(sha256, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
      r.restore(saved);
      CHECK(field(r.read(sr), 16, 6) == (words == 0 ? 33 : 32));
      r.feed(bytes(message.begin() + at, message.end()));
      r.finish(message.size());
      r.settle();
      CHECK(r.digest(sha512) == unhex(find_digest(sha512, 300).digest));
      CHECK(r.logged.empty());
    }
  }
}

TEST_CASE("an HMAC saved in its message phase and after it resumes after another HMAC")
{
  struct resume
  {
    hmac_case const* c;
    std::size_t      at;
    std::size_t      csrs;
  };
  std::array const cases{
      resume{&find_hmac(sha3_512, 72, 200), 76,  88 },
      resume{&find_hmac(sha3_256, 40, 300), 140, 72 },
      resume{&find_hmac(sha512,   20, 300), 132, 103},
  };
  for(auto const& [c, at, csrs] : cases)
  {
    for(auto const after : {false, true})
    {
      CAPTURE(c->algo);
      CAPTURE(after);
      rig const  r;
      auto const key   = pattern(c->key, 1);
      auto const data  = pattern(c->data, 2);
      auto const split = after ? data.size() : at;
      r.hmac_begin(c->algo, c->long_key);
      r.phase(key);
      if(after)
      {
        r.phase(data);
      }
      else
      {
        r.feed(bytes(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(split)));
        r.settle();
      }
      auto const saved = r.save(csrs);
      CHECK(r.hash(sha256, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
      r.restore(saved);
      if(not after)
      {
        r.feed(bytes(data.begin() + static_cast<std::ptrdiff_t>(split), data.end()));
        r.finish(data.size());
        r.settle();
      }
      r.phase(key);
      CHECK(r.digest(c->algo) == unhex(c->digest));
      CHECK(r.logged.empty());
    }
  }
}

namespace
{
constexpr std::uint32_t gpdma_channel7 = 0x4002'03D0;
constexpr std::uint32_t ch_cfcr        = 0x0C;
constexpr std::uint32_t ch_csr         = 0x10;
constexpr std::uint32_t ch_ccr         = 0x14;
constexpr std::uint32_t ch_ctr1        = 0x40;
constexpr std::uint32_t ch_ctr2        = 0x44;
constexpr std::uint32_t ch_cbr1        = 0x48;
constexpr std::uint32_t ch_csar        = 0x4C;
constexpr std::uint32_t ch_cdar        = 0x50;
constexpr std::uint32_t ch_cllr        = 0x7C;
constexpr std::uint32_t ccr_en         = 1u << 0;
constexpr std::uint32_t ccr_reset      = 1u << 1;
constexpr std::uint32_t ccr_susp       = 1u << 2;
constexpr std::uint32_t csr_suspf      = 1u << 13;
constexpr std::uint32_t hash_request   = 111;
constexpr std::uint32_t staged         = 0x2001'0000;

struct dma_feed
{
  void stage(bytes const& data) const
  {
    for(std::size_t at = 0; at < data.size(); at += 4)
    {
      std::uint32_t word = 0;
      for(std::size_t byte = 0; byte < 4 and at + byte < data.size(); ++byte)
      {
        word |= std::uint32_t{data[at + byte]} << (8 * byte);
      }
      REQUIRE(
          r.f->target->mem().write(staged + static_cast<std::uint32_t>(at), 4, word).has_value());
    }
  }

  void put(std::uint32_t offset, std::uint32_t value) const
  {
    REQUIRE(r.f->target->mem().write(gpdma_channel7 + offset, 4, value).has_value());
  }

  [[nodiscard]] auto get(std::uint32_t offset) const -> std::uint32_t
  {
    auto const value = r.f->target->mem().read(gpdma_channel7 + offset, 4);
    REQUIRE(value.has_value());
    return *value;
  }

  void start(std::uint32_t from, std::uint32_t words)
  {
    first = from;
    count = words;
    put(ch_cfcr, 0x7F00);
    put(ch_ctr1, 1u << 3 | 2u | 2u << 16 | (beats - 1) << 4 | (beats - 1) << 20);
    put(ch_ctr2, 1u << 10 | hash_request);
    put(ch_cbr1, 4 * words);
    put(ch_csar, staged + 4 * from);
    put(ch_cdar, hash_base + din);
    put(ch_cllr, 0);
    put(ch_ccr, ccr_en);
  }

  [[nodiscard]] auto sent() const -> std::uint32_t
  {
    return first + count - (get(ch_cbr1) & 0xFFFFu) / 4;
  }

  [[nodiscard]] auto enabled() const -> bool
  {
    return (get(ch_ccr) & ccr_en) != 0;
  }

  template <typename Done> [[nodiscard]] auto until(Done done) const -> bool
  {
    for(int step = 0; step < 4000; ++step)
    {
      if(done())
      {
        return true;
      }
      r.until(r.now() + r.cycle_ps());
    }
    return done();
  }

  void abort() const
  {
    put(ch_ccr, get(ch_ccr) | ccr_susp);
    REQUIRE(until([&] { return (get(ch_csr) & csr_suspf) != 0; }));
    put(ch_ccr, get(ch_ccr) | ccr_reset);
    CHECK_FALSE(enabled());
  }

  rig const&    r;
  std::uint32_t beats{1};
  std::uint32_t first{};
  std::uint32_t count{};
};
}

TEST_CASE("a DMA suspend mid SHA-512 block keeps the words of the started burst in CSR20 to CSR23")
{
  constexpr std::uint32_t sr_dmas = 1u << 2;
  auto const              message = pattern(300);
  rig const               r;
  dma_feed                d{r, 4};
  auto const              control = (sha512 << 17) | (2u << 4);
  d.stage(message);
  r.write(str, 0);
  r.write(cr, cr_init | cr_dmae | control);
  d.start(0, 75);
  REQUIRE(d.until([&] { return d.sent() >= 21; }));
  REQUIRE(d.sent() < 32);
  r.write(cr, control);
  REQUIRE(d.until([&] { return (r.read(sr) & sr_dmas) == 0; }));
  auto const held = d.sent();
  r.until(r.now() + 200 * r.cycle_ps());
  CHECK(d.sent() == held);
  d.abort();
  REQUIRE(d.until([&] { return (r.read(sr) & sr_dinis) != 0; }));
  CHECK(held == 36);
  CHECK(field(r.read(sr), 9, 6) == 4);
  auto const saved = r.save(91);
  CHECK(field(saved.csr[1], 8, 6) == 4);
  CHECK(r.hash(sha256, text("abc")) == unhex(find_digest(sha256, 3, "abc").digest));
  r.restore(saved);
  CHECK((r.read(sr) & sr_dmas) == 0);
  d.start(held, 75 - held);
  r.write(cr, saved.cr | cr_dmae);
  r.settle();
  CHECK_FALSE(d.enabled());
  CHECK(r.digest(sha512) == unhex(find_digest(sha512, 300).digest));
  CHECK(r.logged.empty());
}

TEST_CASE("a context saved under one algorithm is refused under another")
{
  rig const r{false};
  r.begin(sha3_256);
  r.feed(pattern(140));
  r.settle();
  auto const saved = r.save(72);
  r.begin(sha3_512);
  r.write(csr0, saved.csr[0]);
  r.write(csr0 + 4, saved.csr[1]);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "CSR1");
}

TEST_CASE("a SHA-512 context is refused under SHA2-256 whose low ALGO bits are equal")
{
  rig const r{false};
  r.begin(sha512);
  r.feed(pattern(132));
  r.settle();
  auto const saved = r.save(91);
  r.begin(sha256);
  r.write(csr0, saved.csr[0]);
  r.write(csr0 + 4, saved.csr[1]);
  REQUIRE(r.logged.size() == 1);
  CHECK(r.logged[0].reg == "CSR1");
}

TEST_CASE("a snapshot taken after a SHAKE digest resumes the squeeze elsewhere")
{
  rig const r;
  r.begin(shake256);
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  auto const saved    = r.f->target->snapshot();
  auto const expected = unhex(shakes[2].digest);
  auto const check    = [&](rig const& at) {
    CHECK(at.words(136) == bytes(expected.begin(), expected.begin() + 136));
    at.write(str, str_dcal);
    at.settle();
    CHECK(at.words(136) == bytes(expected.begin() + 136, expected.begin() + 272));
  };
  check(r);
  r.f->target->restore(saved);
  check(r);
  rig const other;
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  check(other);
}

TEST_CASE("HASHRST drops the sponge of a SHA-3 hash in the middle of a message")
{
  rig const r;
  r.begin(sha3_256);
  r.feed(pattern(140));
  REQUIRE(saved_integer(*r.f, "HASH", "lanes.0").value_or(0) != 0);
  r.settle();
  rcc_reset_pulse(*r.f, "AHB2RSTR", "HASHRST");
  CHECK(saved_integer(*r.f, "HASH", "lanes.0") == 0);
  CHECK(saved_integer(*r.f, "HASH", "count") == 0);
  CHECK(r.read(cr) == 0);
  CHECK(r.hash(sha3_256, text("abc")) == unhex(find_digest(sha3_256, 3, "abc").digest));
}

TEST_CASE("HASHRST drops the squeeze of a SHAKE digest")
{
  rig const r;
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  REQUIRE(saved_integer(*r.f, "HASH", "squeezing") == 1);
  rcc_reset_pulse(*r.f, "AHB2RSTR", "HASHRST");
  CHECK(saved_integer(*r.f, "HASH", "squeezing") == 0);
  CHECK(saved_integer(*r.f, "HASH", "lanes.0") == 0);
}

TEST_CASE("HASHRST in the middle of a SHAKE squeeze leaves no stale completion for a shorter hash")
{
  rig const r;
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  r.write(str, str_dcal);
  auto const stale = r.now() + 66 * r.cycle_ps();
  r.until(r.now() + 20 * r.cycle_ps());
  rcc_reset_pulse(*r.f, "AHB2RSTR", "HASHRST");
  r.begin(sha3_512);
  r.feed(text("abc"));
  r.finish(3);
  auto const end = r.now() + 42 * r.cycle_ps();
  REQUIRE(end < stale);
  r.until(end);
  REQUIRE((r.read(sr) & (sr_busy | sr_dcis)) == sr_dcis);
  r.write(sr, 0);
  CHECK((r.read(sr) & (sr_dinis | sr_dcis)) == 0);
  r.until(stale + r.cycle_ps());
  CHECK((r.read(sr) & (sr_busy | sr_dinis | sr_dcis)) == 0);
}

TEST_CASE("HASHRST in the middle of a SHAKE squeeze leaves no stale BUSY end for the next hash")
{
  rig const r;
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  r.settle();
  r.write(str, str_dcal);
  auto const stale = r.now() + 66 * r.cycle_ps();
  r.until(r.now() + 20 * r.cycle_ps());
  rcc_reset_pulse(*r.f, "AHB2RSTR", "HASHRST");
  CHECK(saved_integer(*r.f, "HASH", "squeezing") == 0);
  CHECK(saved_integer(*r.f, "HASH", "lanes.0") == 0);
  r.begin(shake128);
  r.feed(text("abc"));
  r.finish(3);
  auto const end = r.now() + 66 * r.cycle_ps();
  REQUIRE(end > stale);
  r.until(stale + r.cycle_ps());
  CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_busy);
  r.until(end);
  CHECK((r.read(sr) & (sr_busy | sr_dcis)) == sr_dcis);
  CHECK(r.words(168) == unhex(shakes[0].digest.substr(0, 336)));
}
