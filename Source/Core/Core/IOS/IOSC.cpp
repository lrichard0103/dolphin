// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/IOSC.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include <mbedtls/md.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha1.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/ec.h"
#include "Common/File.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/Formats.h"

namespace
{
#pragma pack(push, 1)
/*
 * Structs for keys.bin taken from:
 *
 * mini - a Free Software replacement for the Nintendo/BroadOn IOS.
 * crypto hardware support
 *
 * Copyright (C) 2008, 2009 Haxx Enterprises <bushing@gmail.com>
 * Copyright (C) 2008, 2009 Sven Peter <svenpeter@gmail.com>
 * Copyright (C) 2008, 2009 Hector Martin "marcan" <marcan@marcansoft.com>
 *
 * # This code is licensed to you under the terms of the GNU GPL, version 2;
 * # see file COPYING or http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt
 */
struct BootMiiKeyDump
{
  std::array<char, 256> creator;
  std::array<u8, 20> boot1_hash;  // 0x100
  std::array<u8, 16> common_key;  // 0x114
  u32 ng_id;                      // 0x124
  union
  {
    struct
    {
      std::array<u8, 0x1e> ng_priv;  // 0x128
      std::array<u8, 0x12> pad1;
    };
    struct
    {
      std::array<u8, 0x1c> pad2;
      std::array<u8, 0x14> nand_hmac;  // 0x144
    };
  };
  std::array<u8, 16> nand_key;      // 0x158
  std::array<u8, 16> backup_key;    // 0x168
  u32 unk1;                         // 0x178
  u32 unk2;                         // 0x17C
  std::array<u8, 0x80> eeprom_pad;  // 0x180

  u32 ms_id;                 // 0x200
  u32 ca_id;                 // 0x204
  u32 ng_key_id;             // 0x208
  IOS::ECCSignature ng_sig;  // 0x20c
  struct Counter
  {
    u8 boot2version;
    u8 unknown1;
    u8 unknown2;
    u8 pad;
    u32 update_tag;
    u16 checksum;
  };
  std::array<Counter, 2> counters;  // 0x248
  std::array<u8, 0x18> fill;        // 0x25c
  std::array<u8, 16> korean_key;    // 0x274
  std::array<u8, 0x74> pad3;        // 0x284
  std::array<u16, 2> prng_seed;     // 0x2F8
  std::array<u8, 4> pad4;           // 0x2FC
  std::array<u8, 0x100> crack_pad;  // 0x300
};
static_assert(sizeof(BootMiiKeyDump) == 0x400, "Wrong size");
#pragma pack(pop)
}  // end of anonymous namespace

namespace IOS
{
namespace HLE
{
constexpr u32 DEFAULT_DEVICE_ID = 0x0403AC68;
constexpr u32 DEFAULT_KEY_ID = 0x6AAB8C59;

constexpr std::array<u8, 30> DEFAULT_PRIVATE_KEY = {{
    0x00, 0xAB, 0xEE, 0xC1, 0xDD, 0xB4, 0xA6, 0x16, 0x6B, 0x70, 0xFD, 0x7E, 0x56, 0x67, 0x70,
    0x57, 0x55, 0x27, 0x38, 0xA3, 0x26, 0xC5, 0x46, 0x16, 0xF7, 0x62, 0xC9, 0xED, 0x73, 0xF2,
}};

// clang-format off
constexpr ECCSignature DEFAULT_SIGNATURE = {{
    // R
    0x00, 0xD8, 0x81, 0x63, 0xB2, 0x00, 0x6B, 0x0B, 0x54, 0x82, 0x88, 0x63, 0x81, 0x1C, 0x00, 0x71,
    0x12, 0xED, 0xB7, 0xFD, 0x21, 0xAB, 0x0E, 0x50, 0x0E, 0x1F, 0xBF, 0x78, 0xAD, 0x37,
    // S
    0x00, 0x71, 0x8D, 0x82, 0x41, 0xEE, 0x45, 0x11, 0xC7, 0x3B, 0xAC, 0x08, 0xB6, 0x83, 0xDC, 0x05,
    0xB8, 0xA8, 0x90, 0x1F, 0xA8, 0x2A, 0x0E, 0x4E, 0x76, 0xEF, 0x44, 0x72, 0x99, 0xF8,
}};
// clang-format on

const std::map<std::pair<IOSC::ObjectType, IOSC::ObjectSubType>, size_t> s_type_to_size_map = {{
    {{IOSC::TYPE_SECRET_KEY, IOSC::SUBTYPE_AES128}, 16},
    {{IOSC::TYPE_SECRET_KEY, IOSC::SUBTYPE_MAC}, 20},
    {{IOSC::TYPE_SECRET_KEY, IOSC::SUBTYPE_ECC233}, 30},
    {{IOSC::TYPE_PUBLIC_KEY, IOSC::SUBTYPE_RSA2048}, 256},
    {{IOSC::TYPE_PUBLIC_KEY, IOSC::SUBTYPE_RSA4096}, 512},
    {{IOSC::TYPE_PUBLIC_KEY, IOSC::SUBTYPE_ECC233}, 60},
    {{IOSC::TYPE_DATA, IOSC::SUBTYPE_DATA}, 0},
    {{IOSC::TYPE_DATA, IOSC::SUBTYPE_VERSION}, 0},
}};

static size_t GetSizeForType(IOSC::ObjectType type, IOSC::ObjectSubType subtype)
{
  const auto iterator = s_type_to_size_map.find({type, subtype});
  return iterator != s_type_to_size_map.end() ? iterator->second : 0;
}

IOSC::IOSC(ConsoleType console_type)
{
  LoadDefaultEntries(console_type);
  LoadEntries();
}

IOSC::~IOSC() = default;

ReturnCode IOSC::CreateObject(Handle* handle, ObjectType type, ObjectSubType subtype, u32 pid)
{
  auto iterator = FindFreeEntry();
  if (iterator == m_key_entries.end())
    return IOSC_FAIL_ALLOC;

  iterator->in_use = true;
  iterator->type = type;
  iterator->subtype = subtype;
  iterator->owner_mask = 1 << pid;

  *handle = GetHandleFromIterator(iterator);
  return IPC_SUCCESS;
}

ReturnCode IOSC::DeleteObject(Handle handle, u32 pid)
{
  if (IsDefaultHandle(handle) || !HasOwnership(handle, pid))
    return IOSC_EACCES;

  KeyEntry* entry = FindEntry(handle);
  if (!entry)
    return IOSC_EINVAL;
  entry->in_use = false;
  entry->data.clear();
  return IPC_SUCCESS;
}

constexpr size_t AES128_KEY_SIZE = 0x10;
ReturnCode IOSC::ImportSecretKey(Handle dest_handle, Handle decrypt_handle, u8* iv,
                                 const u8* encrypted_key, u32 pid)
{
  std::array<u8, AES128_KEY_SIZE> decrypted_key;
  const ReturnCode ret =
      Decrypt(decrypt_handle, iv, encrypted_key, AES128_KEY_SIZE, decrypted_key.data(), pid);
  if (ret != IPC_SUCCESS)
    return ret;

  return ImportSecretKey(dest_handle, decrypted_key.data(), pid);
}

ReturnCode IOSC::ImportSecretKey(Handle dest_handle, const u8* decrypted_key, u32 pid)
{
  if (!HasOwnership(dest_handle, pid) || IsDefaultHandle(dest_handle))
    return IOSC_EACCES;

  KeyEntry* dest_entry = FindEntry(dest_handle);
  if (!dest_entry)
    return IOSC_EINVAL;

  // TODO: allow other secret key subtypes
  if (dest_entry->type != TYPE_SECRET_KEY || dest_entry->subtype != SUBTYPE_AES128)
    return IOSC_INVALID_OBJTYPE;

  dest_entry->data = std::vector<u8>(decrypted_key, decrypted_key + AES128_KEY_SIZE);
  return IPC_SUCCESS;
}

ReturnCode IOSC::ImportPublicKey(Handle dest_handle, const u8* public_key,
                                 const u8* public_key_exponent, u32 pid)
{
  if (!HasOwnership(dest_handle, pid) || IsDefaultHandle(dest_handle))
    return IOSC_EACCES;

  KeyEntry* dest_entry = FindEntry(dest_handle);
  if (!dest_entry)
    return IOSC_EINVAL;

  if (dest_entry->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  const size_t size = GetSizeForType(dest_entry->type, dest_entry->subtype);
  if (size == 0)
    return IOSC_INVALID_OBJTYPE;

  dest_entry->data.assign(public_key, public_key + size);

  if (dest_entry->subtype == SUBTYPE_RSA2048 || dest_entry->subtype == SUBTYPE_RSA4096)
  {
    ASSERT(public_key_exponent);
    std::memcpy(&dest_entry->misc_data, public_key_exponent, 4);
  }
  return IPC_SUCCESS;
}

ReturnCode IOSC::ComputeSharedKey(Handle dest_handle, Handle private_handle, Handle public_handle,
                                  u32 pid)
{
  if (!HasOwnership(dest_handle, pid) || !HasOwnership(private_handle, pid) ||
      !HasOwnership(public_handle, pid) || IsDefaultHandle(dest_handle))
  {
    return IOSC_EACCES;
  }

  KeyEntry* dest_entry = FindEntry(dest_handle);
  const KeyEntry* private_entry = FindEntry(private_handle);
  const KeyEntry* public_entry = FindEntry(public_handle);
  if (!dest_entry || !private_entry || !public_entry)
    return IOSC_EINVAL;
  if (dest_entry->type != TYPE_SECRET_KEY || dest_entry->subtype != SUBTYPE_AES128 ||
      private_entry->type != TYPE_SECRET_KEY || private_entry->subtype != SUBTYPE_ECC233 ||
      public_entry->type != TYPE_PUBLIC_KEY || public_entry->subtype != SUBTYPE_ECC233)
  {
    return IOSC_INVALID_OBJTYPE;
  }

  // Calculate the ECC shared secret.
  const std::array<u8, 0x3c> shared_secret =
      Common::ec::ComputeSharedSecret(private_entry->data.data(), public_entry->data.data());

  std::array<u8, 20> sha1;
  mbedtls_sha1(shared_secret.data(), shared_secret.size() / 2, sha1.data());

  dest_entry->data.resize(AES128_KEY_SIZE);
  std::copy_n(sha1.cbegin(), AES128_KEY_SIZE, dest_entry->data.begin());
  return IPC_SUCCESS;
}

ReturnCode IOSC::DecryptEncrypt(Common::AES::Mode mode, Handle key_handle, u8* iv, const u8* input,
                                size_t size, u8* output, u32 pid) const
{
  if (!HasOwnership(key_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* entry = FindEntry(key_handle);
  if (!entry)
    return IOSC_EINVAL;
  if (entry->type != TYPE_SECRET_KEY || entry->subtype != SUBTYPE_AES128)
    return IOSC_INVALID_OBJTYPE;

  if (entry->data.size() != AES128_KEY_SIZE)
    return IOSC_FAIL_INTERNAL;

  const std::vector<u8> data =
      Common::AES::DecryptEncrypt(entry->data.data(), iv, input, size, mode);

  std::memcpy(output, data.data(), data.size());
  return IPC_SUCCESS;
}

ReturnCode IOSC::Encrypt(Handle key_handle, u8* iv, const u8* input, size_t size, u8* output,
                         u32 pid) const
{
  return DecryptEncrypt(Common::AES::Mode::Encrypt, key_handle, iv, input, size, output, pid);
}

ReturnCode IOSC::Decrypt(Handle key_handle, u8* iv, const u8* input, size_t size, u8* output,
                         u32 pid) const
{
  return DecryptEncrypt(Common::AES::Mode::Decrypt, key_handle, iv, input, size, output, pid);
}

ReturnCode IOSC::VerifyPublicKeySign(const std::array<u8, 20>& sha1, Handle signer_handle,
                                     const std::vector<u8>& signature, u32 pid) const
{
  if (!HasOwnership(signer_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* entry = FindEntry(signer_handle, SearchMode::IncludeRootKey);
  if (!entry)
    return IOSC_EINVAL;

  // TODO: add support for keypair entries.
  if (entry->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  switch (entry->subtype)
  {
  case SUBTYPE_RSA2048:
  case SUBTYPE_RSA4096:
  {
    const size_t expected_key_size = entry->subtype == SUBTYPE_RSA2048 ? 0x100 : 0x200;
    ASSERT(entry->data.size() == expected_key_size);
    ASSERT(signature.size() == expected_key_size);

    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);
    Common::ScopeGuard context_guard{[&rsa] { mbedtls_rsa_free(&rsa); }};

    mbedtls_mpi_read_binary(&rsa.N, entry->data.data(), entry->data.size());
    mbedtls_mpi_read_binary(&rsa.E, reinterpret_cast<const u8*>(&entry->misc_data), 4);
    rsa.len = entry->data.size();

    const int ret = mbedtls_rsa_pkcs1_verify(&rsa, nullptr, nullptr, MBEDTLS_RSA_PUBLIC,
                                             MBEDTLS_MD_SHA1, 0, sha1.data(), signature.data());
    if (ret != 0)
    {
      WARN_LOG(IOS, "VerifyPublicKeySign: RSA verification failed (error %d)", ret);
      return IOSC_FAIL_CHECKVALUE;
    }

    return IPC_SUCCESS;
  }
  case SUBTYPE_ECC233:
    ERROR_LOG(IOS, "VerifyPublicKeySign: SUBTYPE_ECC233 is unimplemented");
  // [[fallthrough]]
  default:
    return IOSC_INVALID_OBJTYPE;
  }
}

ReturnCode IOSC::ImportCertificate(const IOS::ES::CertReader& cert, Handle signer_handle,
                                   Handle dest_handle, u32 pid)
{
  if (!HasOwnership(signer_handle, pid) || !HasOwnership(dest_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* signer_entry = FindEntry(signer_handle, SearchMode::IncludeRootKey);
  const KeyEntry* dest_entry = FindEntry(dest_handle, SearchMode::IncludeRootKey);
  if (!signer_entry || !dest_entry)
    return IOSC_EINVAL;

  if (signer_entry->type != TYPE_PUBLIC_KEY || dest_entry->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  if (!cert.IsValid())
    return IOSC_INVALID_FORMAT;

  const std::vector<u8> signature = cert.GetSignatureData();
  if (VerifyPublicKeySign(cert.GetSha1(), signer_handle, signature, pid) != IPC_SUCCESS)
    return IOSC_FAIL_CHECKVALUE;

  const std::vector<u8> public_key = cert.GetPublicKey();
  const bool is_rsa = cert.GetSignatureType() != SignatureType::ECC;
  const u8* exponent = is_rsa ? (public_key.data() + public_key.size() - 4) : nullptr;
  return ImportPublicKey(dest_handle, public_key.data(), exponent, pid);
}

ReturnCode IOSC::GetOwnership(Handle handle, u32* owner) const
{
  const KeyEntry* entry = FindEntry(handle);
  if (entry && entry->in_use)
  {
    *owner = entry->owner_mask;
    return IPC_SUCCESS;
  }
  return IOSC_EINVAL;
}

ReturnCode IOSC::SetOwnership(Handle handle, u32 new_owner, u32 pid)
{
  if (!HasOwnership(handle, pid))
    return IOSC_EACCES;

  KeyEntry* entry = FindEntry(handle);
  if (!entry)
    return IOSC_EINVAL;

  const u32 mask_with_current_pid = 1 << pid;
  const u32 mask = entry->owner_mask | mask_with_current_pid;
  if (mask != mask_with_current_pid)
    return IOSC_EACCES;
  entry->owner_mask = (new_owner & ~7) | mask;
  return IPC_SUCCESS;
}

bool IOSC::IsUsingDefaultId() const
{
  return GetDeviceId() == DEFAULT_DEVICE_ID;
}

u32 IOSC::GetDeviceId() const
{
  return m_key_entries[HANDLE_CONSOLE_ID].misc_data;
}

// Based off of twintig http://git.infradead.org/?p=users/segher/wii.git
// Copyright 2007,2008  Segher Boessenkool  <segher@kernel.crashing.org>
// Licensed under the terms of the GNU GPL, version 2
// http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt
static Certificate MakeBlankSigECCert(const char* signer, const char* name, const u8* private_key,
                                      u32 key_id)
{
  Certificate cert_out{};
  const u32 type = Common::swap32(static_cast<u32>(SignatureType::ECC));
  std::memcpy(cert_out.data(), &type, sizeof(type));
  std::strncpy(reinterpret_cast<char*>(cert_out.data()) + 0x80, signer, 0x40);
  const u32 two = Common::swap32(2);
  std::memcpy(cert_out.data() + 0xc0, &two, sizeof(two));
  std::strncpy(reinterpret_cast<char*>(cert_out.data()) + 0xc4, name, 0x40);
  const u32 swapped_key_id = Common::swap32(key_id);
  std::memcpy(cert_out.data() + 0x104, &swapped_key_id, sizeof(swapped_key_id));
  const std::array<u8, 60> public_key = Common::ec::PrivToPub(private_key);
  std::copy(public_key.cbegin(), public_key.cend(), cert_out.begin() + 0x108);
  return cert_out;
}

Certificate IOSC::GetDeviceCertificate() const
{
  const std::string name = StringFromFormat("NG%08x", GetDeviceId());
  auto cert = MakeBlankSigECCert("Root-CA00000001-MS00000002", name.c_str(),
                                 m_key_entries[HANDLE_CONSOLE_KEY].data.data(), m_console_key_id);
  std::copy(m_console_signature.begin(), m_console_signature.end(), cert.begin() + 4);
  return cert;
}

void IOSC::Sign(u8* sig_out, u8* ap_cert_out, u64 title_id, const u8* data, u32 data_size) const
{
  std::array<u8, 20> hash{};
  std::array<u8, 30> ap_priv{};

  ap_priv[0x1d] = 1;
  // setup random ap_priv here if desired
  // get_rand_bytes(ap_priv, 0x1e);
  // ap_priv[0] &= 1;

  const std::string signer = StringFromFormat("Root-CA00000001-MS00000002-NG%08x", GetDeviceId());
  const std::string name = StringFromFormat("AP%016" PRIx64, title_id);
  const auto cert = MakeBlankSigECCert(signer.c_str(), name.c_str(), ap_priv.data(), 0);
  std::copy(cert.begin(), cert.end(), ap_cert_out);

  mbedtls_sha1(ap_cert_out + 0x80, 0x100, hash.data());
  auto signature = Common::ec::Sign(m_key_entries[HANDLE_CONSOLE_KEY].data.data(), hash.data());
  std::copy(signature.cbegin(), signature.cend(), ap_cert_out + 4);

  mbedtls_sha1(data, data_size, hash.data());
  signature = Common::ec::Sign(ap_priv.data(), hash.data());
  std::copy(signature.cbegin(), signature.cend(), sig_out);
}

constexpr std::array<u8, 512> ROOT_PUBLIC_KEY = {
    {0xF8, 0x24, 0x6C, 0x58, 0xBA, 0xE7, 0x50, 0x03, 0x01, 0xFB, 0xB7, 0xC2, 0xEB, 0xE0, 0x01,
     0x05, 0x71, 0xDA, 0x92, 0x23, 0x78, 0xF0, 0x51, 0x4E, 0xC0, 0x03, 0x1D, 0xD0, 0xD2, 0x1E,
     0xD3, 0xD0, 0x7E, 0xFC, 0x85, 0x20, 0x69, 0xB5, 0xDE, 0x9B, 0xB9, 0x51, 0xA8, 0xBC, 0x90,
     0xA2, 0x44, 0x92, 0x6D, 0x37, 0x92, 0x95, 0xAE, 0x94, 0x36, 0xAA, 0xA6, 0xA3, 0x02, 0x51,
     0x0C, 0x7B, 0x1D, 0xED, 0xD5, 0xFB, 0x20, 0x86, 0x9D, 0x7F, 0x30, 0x16, 0xF6, 0xBE, 0x65,
     0xD3, 0x83, 0xA1, 0x6D, 0xB3, 0x32, 0x1B, 0x95, 0x35, 0x18, 0x90, 0xB1, 0x70, 0x02, 0x93,
     0x7E, 0xE1, 0x93, 0xF5, 0x7E, 0x99, 0xA2, 0x47, 0x4E, 0x9D, 0x38, 0x24, 0xC7, 0xAE, 0xE3,
     0x85, 0x41, 0xF5, 0x67, 0xE7, 0x51, 0x8C, 0x7A, 0x0E, 0x38, 0xE7, 0xEB, 0xAF, 0x41, 0x19,
     0x1B, 0xCF, 0xF1, 0x7B, 0x42, 0xA6, 0xB4, 0xED, 0xE6, 0xCE, 0x8D, 0xE7, 0x31, 0x8F, 0x7F,
     0x52, 0x04, 0xB3, 0x99, 0x0E, 0x22, 0x67, 0x45, 0xAF, 0xD4, 0x85, 0xB2, 0x44, 0x93, 0x00,
     0x8B, 0x08, 0xC7, 0xF6, 0xB7, 0xE5, 0x6B, 0x02, 0xB3, 0xE8, 0xFE, 0x0C, 0x9D, 0x85, 0x9C,
     0xB8, 0xB6, 0x82, 0x23, 0xB8, 0xAB, 0x27, 0xEE, 0x5F, 0x65, 0x38, 0x07, 0x8B, 0x2D, 0xB9,
     0x1E, 0x2A, 0x15, 0x3E, 0x85, 0x81, 0x80, 0x72, 0xA2, 0x3B, 0x6D, 0xD9, 0x32, 0x81, 0x05,
     0x4F, 0x6F, 0xB0, 0xF6, 0xF5, 0xAD, 0x28, 0x3E, 0xCA, 0x0B, 0x7A, 0xF3, 0x54, 0x55, 0xE0,
     0x3D, 0xA7, 0xB6, 0x83, 0x26, 0xF3, 0xEC, 0x83, 0x4A, 0xF3, 0x14, 0x04, 0x8A, 0xC6, 0xDF,
     0x20, 0xD2, 0x85, 0x08, 0x67, 0x3C, 0xAB, 0x62, 0xA2, 0xC7, 0xBC, 0x13, 0x1A, 0x53, 0x3E,
     0x0B, 0x66, 0x80, 0x6B, 0x1C, 0x30, 0x66, 0x4B, 0x37, 0x23, 0x31, 0xBD, 0xC4, 0xB0, 0xCA,
     0xD8, 0xD1, 0x1E, 0xE7, 0xBB, 0xD9, 0x28, 0x55, 0x48, 0xAA, 0xEC, 0x1F, 0x66, 0xE8, 0x21,
     0xB3, 0xC8, 0xA0, 0x47, 0x69, 0x00, 0xC5, 0xE6, 0x88, 0xE8, 0x0C, 0xCE, 0x3C, 0x61, 0xD6,
     0x9C, 0xBB, 0xA1, 0x37, 0xC6, 0x60, 0x4F, 0x7A, 0x72, 0xDD, 0x8C, 0x7B, 0x3E, 0x3D, 0x51,
     0x29, 0x0D, 0xAA, 0x6A, 0x59, 0x7B, 0x08, 0x1F, 0x9D, 0x36, 0x33, 0xA3, 0x46, 0x7A, 0x35,
     0x61, 0x09, 0xAC, 0xA7, 0xDD, 0x7D, 0x2E, 0x2F, 0xB2, 0xC1, 0xAE, 0xB8, 0xE2, 0x0F, 0x48,
     0x92, 0xD8, 0xB9, 0xF8, 0xB4, 0x6F, 0x4E, 0x3C, 0x11, 0xF4, 0xF4, 0x7D, 0x8B, 0x75, 0x7D,
     0xFE, 0xFE, 0xA3, 0x89, 0x9C, 0x33, 0x59, 0x5C, 0x5E, 0xFD, 0xEB, 0xCB, 0xAB, 0xE8, 0x41,
     0x3E, 0x3A, 0x9A, 0x80, 0x3C, 0x69, 0x35, 0x6E, 0xB2, 0xB2, 0xAD, 0x5C, 0xC4, 0xC8, 0x58,
     0x45, 0x5E, 0xF5, 0xF7, 0xB3, 0x06, 0x44, 0xB4, 0x7C, 0x64, 0x06, 0x8C, 0xDF, 0x80, 0x9F,
     0x76, 0x02, 0x5A, 0x2D, 0xB4, 0x46, 0xE0, 0x3D, 0x7C, 0xF6, 0x2F, 0x34, 0xE7, 0x02, 0x45,
     0x7B, 0x02, 0xA4, 0xCF, 0x5D, 0x9D, 0xD5, 0x3C, 0xA5, 0x3A, 0x7C, 0xA6, 0x29, 0x78, 0x8C,
     0x67, 0xCA, 0x08, 0xBF, 0xEC, 0xCA, 0x43, 0xA9, 0x57, 0xAD, 0x16, 0xC9, 0x4E, 0x1C, 0xD8,
     0x75, 0xCA, 0x10, 0x7D, 0xCE, 0x7E, 0x01, 0x18, 0xF0, 0xDF, 0x6B, 0xFE, 0xE5, 0x1D, 0xDB,
     0xD9, 0x91, 0xC2, 0x6E, 0x60, 0xCD, 0x48, 0x58, 0xAA, 0x59, 0x2C, 0x82, 0x00, 0x75, 0xF2,
     0x9F, 0x52, 0x6C, 0x91, 0x7C, 0x6F, 0xE5, 0x40, 0x3E, 0xA7, 0xD4, 0xA5, 0x0C, 0xEC, 0x3B,
     0x73, 0x84, 0xDE, 0x88, 0x6E, 0x82, 0xD2, 0xEB, 0x4D, 0x4E, 0x42, 0xB5, 0xF2, 0xB1, 0x49,
     0xA8, 0x1E, 0xA7, 0xCE, 0x71, 0x44, 0xDC, 0x29, 0x94, 0xCF, 0xC4, 0x4E, 0x1F, 0x91, 0xCB,
     0xD4, 0x95}};

void IOSC::LoadDefaultEntries(ConsoleType console_type)
{
  m_key_entries[HANDLE_CONSOLE_KEY] = {
      TYPE_SECRET_KEY, SUBTYPE_ECC233, {DEFAULT_PRIVATE_KEY.begin(), DEFAULT_PRIVATE_KEY.end()}, 3};
  m_console_signature = DEFAULT_SIGNATURE;
  m_console_key_id = DEFAULT_KEY_ID;
  m_key_entries[HANDLE_CONSOLE_ID] = {TYPE_DATA, SUBTYPE_DATA, {}, DEFAULT_DEVICE_ID, 0xFFFFFFF};
  m_key_entries[HANDLE_FS_KEY] = {TYPE_SECRET_KEY, SUBTYPE_AES128, std::vector<u8>(16), 5};
  m_key_entries[HANDLE_FS_MAC] = {TYPE_SECRET_KEY, SUBTYPE_MAC, std::vector<u8>(20), 5};

  switch (console_type)
  {
  case ConsoleType::Retail:
    m_key_entries[HANDLE_COMMON_KEY] = {TYPE_SECRET_KEY,
                                        SUBTYPE_AES128,
                                        {{0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4, 0x48,
                                          0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7}},
                                        3};
    break;
  case ConsoleType::RVT:
    m_key_entries[HANDLE_COMMON_KEY] = {TYPE_SECRET_KEY,
                                        SUBTYPE_AES128,
                                        {{0xa1, 0x60, 0x4a, 0x6a, 0x71, 0x23, 0xb5, 0x29, 0xae,
                                          0x8b, 0xec, 0x32, 0xc8, 0x16, 0xfc, 0xaa}},
                                        3};
    break;
  default:
    ASSERT_MSG(IOS, false, "Unknown console type");
    break;
  }

  m_key_entries[HANDLE_PRNG_KEY] = {TYPE_SECRET_KEY, SUBTYPE_AES128,
                                    std::vector<u8>(AES128_KEY_SIZE), 3};

  m_key_entries[HANDLE_SD_KEY] = {TYPE_SECRET_KEY,
                                  SUBTYPE_AES128,
                                  {{0xab, 0x01, 0xb9, 0xd8, 0xe1, 0x62, 0x2b, 0x08, 0xaf, 0xba,
                                    0xd8, 0x4d, 0xbf, 0xc2, 0xa5, 0x5d}},
                                  3};

  m_key_entries[HANDLE_BOOT2_VERSION] = {TYPE_DATA, SUBTYPE_VERSION, {}, 3};
  m_key_entries[HANDLE_UNKNOWN_8] = {TYPE_DATA, SUBTYPE_VERSION, {}, 3};
  m_key_entries[HANDLE_UNKNOWN_9] = {TYPE_DATA, SUBTYPE_VERSION, {}, 3};
  m_key_entries[HANDLE_FS_VERSION] = {TYPE_DATA, SUBTYPE_VERSION, {}, 3};

  m_key_entries[HANDLE_NEW_COMMON_KEY] = {TYPE_SECRET_KEY,
                                          SUBTYPE_AES128,
                                          {{0x63, 0xb8, 0x2b, 0xb4, 0xf4, 0x61, 0x4e, 0x2e, 0x13,
                                            0xf2, 0xfe, 0xfb, 0xba, 0x4c, 0x9b, 0x7e}},
                                          3};

  m_root_key_entry = {TYPE_PUBLIC_KEY, SUBTYPE_RSA4096,
                      std::vector<u8>(ROOT_PUBLIC_KEY.begin(), ROOT_PUBLIC_KEY.end()),
                      Common::swap32(0x00010001), 0};
}

void IOSC::LoadEntries()
{
  File::IOFile file{File::GetUserPath(D_WIIROOT_IDX) + "/keys.bin", "rb"};
  if (!file)
  {
    WARN_LOG(IOS, "keys.bin could not be found. Default values will be used.");
    return;
  }

  BootMiiKeyDump dump;
  if (!file.ReadBytes(&dump, sizeof(dump)))
  {
    ERROR_LOG(IOS, "Failed to read from keys.bin.");
    return;
  }

  m_key_entries[HANDLE_CONSOLE_KEY].data = {dump.ng_priv.begin(), dump.ng_priv.end()};
  m_console_signature = dump.ng_sig;
  m_console_key_id = Common::swap32(dump.ng_key_id);
  m_key_entries[HANDLE_CONSOLE_ID].misc_data = Common::swap32(dump.ng_id);
  m_key_entries[HANDLE_FS_KEY].data = {dump.nand_key.begin(), dump.nand_key.end()};
  m_key_entries[HANDLE_FS_MAC].data = {dump.nand_hmac.begin(), dump.nand_hmac.end()};
  m_key_entries[HANDLE_PRNG_KEY].data = {dump.backup_key.begin(), dump.backup_key.end()};
  m_key_entries[HANDLE_BOOT2_VERSION].misc_data = dump.counters[0].boot2version;
}

IOSC::KeyEntry::KeyEntry() = default;

IOSC::KeyEntry::KeyEntry(ObjectType type_, ObjectSubType subtype_, std::vector<u8>&& data_,
                         u32 misc_data_, u32 owner_mask_)
    : in_use(true), type(type_), subtype(subtype_), data(std::move(data_)), misc_data(misc_data_),
      owner_mask(owner_mask_)
{
}

IOSC::KeyEntry::KeyEntry(ObjectType type_, ObjectSubType subtype_, std::vector<u8>&& data_,
                         u32 owner_mask_)
    : KeyEntry(type_, subtype_, std::move(data_), {}, owner_mask_)
{
}

IOSC::KeyEntries::iterator IOSC::FindFreeEntry()
{
  return std::find_if(m_key_entries.begin(), m_key_entries.end(),
                      [](const auto& entry) { return !entry.in_use; });
}

IOSC::KeyEntry* IOSC::FindEntry(Handle handle)
{
  return handle < m_key_entries.size() ? &m_key_entries[handle] : nullptr;
}
const IOSC::KeyEntry* IOSC::FindEntry(Handle handle, SearchMode mode) const
{
  if (mode == SearchMode::IncludeRootKey && handle == HANDLE_ROOT_KEY)
    return &m_root_key_entry;
  return handle < m_key_entries.size() ? &m_key_entries[handle] : nullptr;
}

IOSC::Handle IOSC::GetHandleFromIterator(IOSC::KeyEntries::iterator iterator) const
{
  ASSERT(iterator != m_key_entries.end());
  return static_cast<Handle>(iterator - m_key_entries.begin());
}

bool IOSC::HasOwnership(Handle handle, u32 pid) const
{
  u32 owner_mask;
  return handle == HANDLE_ROOT_KEY ||
         (GetOwnership(handle, &owner_mask) == IPC_SUCCESS && ((1 << pid) & owner_mask) != 0);
}

bool IOSC::IsDefaultHandle(Handle handle) const
{
  constexpr Handle last_default_handle = HANDLE_NEW_COMMON_KEY;
  return handle <= last_default_handle || handle == HANDLE_ROOT_KEY;
}

void IOSC::DoState(PointerWrap& p)
{
  for (auto& entry : m_key_entries)
    entry.DoState(p);
}

void IOSC::KeyEntry::DoState(PointerWrap& p)
{
  p.Do(in_use);
  p.Do(type);
  p.Do(subtype);
  p.Do(data);
  p.Do(owner_mask);
}
}  // namespace HLE
}  // namespace IOS
