#pragma once

#include <Windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <shlwapi.h>
#include <mscat.h>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <algorithm>
#include <vector>
#include <string_view>
#include <future>

#include "_filtered_signatures.hh"

enum class SignatureStatus {
    Signed,
    Unsigned,
    NotFound,
    Cheat,
    Fake
};

inline bool operator==(SignatureStatus lhs, SignatureStatus rhs) { return static_cast<int>(lhs) == static_cast<int>(rhs); }
inline bool operator!=(SignatureStatus lhs, SignatureStatus rhs) { return !(lhs == rhs); }

static std::unordered_map<std::wstring, SignatureStatus> g_signatureCache;
static std::shared_mutex g_signatureMutex;
static std::unordered_map<std::string, SignatureStatus> g_winTrustCache;
static std::shared_mutex g_winTrustMutex;

static bool ReadFileHeader(const std::wstring& path, BYTE* buffer, DWORD bytesToRead, DWORD& outRead)
{
    outRead = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD read = 0;
    BOOL ok = ReadFile(h, buffer, bytesToRead, &read, nullptr);
    CloseHandle(h);

    if (!ok || read == 0)
        return false;

    outRead = read;
    return true;
}

std::string ComputeFileHeaderHash(const std::wstring& path) {
    BYTE buf[1024] = { 0 };
    DWORD read = 0;
    if (!ReadFileHeader(path, buf, sizeof(buf), read) || read == 0) return "";

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return "";
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    if (!CryptHashData(hHash, buf, read, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }
    DWORD hashLen = 20;
    BYTE hash[20];
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    std::string hashStr;
    hashStr.reserve(40);
    for (BYTE b : hash) {
        char hex[3];
        sprintf_s(hex, "%02x", b);
        hashStr += hex;
    }
    return hashStr;
}

bool IsPEFile(const std::wstring& path)
{
    BYTE buf[0x200] = { 0 };
    DWORD read = 0;
    if (!ReadFileHeader(path, buf, sizeof(buf), read) || read < 0x40)
        return false;

    if (buf[0] != 'M' || buf[1] != 'Z')
        return false;

    DWORD e_lfanew = *reinterpret_cast<DWORD*>(buf + 0x3C);
    if (e_lfanew + 0x18 + sizeof(IMAGE_FILE_HEADER) > read)
        return false;

    BYTE* peHeader = buf + e_lfanew;
    if (!(peHeader[0] == 'P' && peHeader[1] == 'E' && peHeader[2] == 0 && peHeader[3] == 0))
        return false;

    auto* fileHeader = reinterpret_cast<IMAGE_FILE_HEADER*>(peHeader + 4);
    return fileHeader->NumberOfSections > 0 && fileHeader->NumberOfSections <= 96;
}

std::optional<PCCERT_CONTEXT> GetSignerCertificate(const std::wstring& filePath)
{
    HCERTSTORE hStore = nullptr;
    HCRYPTMSG hMsg = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, filePath.c_str(),
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0,
        nullptr, nullptr, nullptr, &hStore, &hMsg, nullptr)) return std::nullopt;

    DWORD signerInfoSize = 0;
    CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerInfoSize);
    std::unique_ptr<BYTE[]> buffer(new BYTE[signerInfoSize]);
    CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, buffer.get(), &signerInfoSize);
    auto* pSignerInfo = reinterpret_cast<CMSG_SIGNER_INFO*>(buffer.get());

    CERT_INFO certInfo{};
    certInfo.Issuer = pSignerInfo->Issuer;
    certInfo.SerialNumber = pSignerInfo->SerialNumber;

    PCCERT_CONTEXT pCertContext = CertFindCertificateInStore(hStore,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &certInfo, nullptr);

    if (pCertContext) {
        CertCloseStore(hStore, 0);
        CryptMsgClose(hMsg);
        return pCertContext;
    }
    CertCloseStore(hStore, 0);
    CryptMsgClose(hMsg);
    return std::nullopt;
}

bool VerifyFileViaCatalog(const std::wstring& filePath)
{
    HANDLE hCatAdmin = nullptr;
    if (!CryptCATAdminAcquireContext(&hCatAdmin, nullptr, 0))
        return false;

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return false;
    }

    DWORD dwHashSize = 0;
    if (!CryptCATAdminCalcHashFromFileHandle(hFile, &dwHashSize, nullptr, 0)) {
        CloseHandle(hFile);
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return false;
    }

    std::vector<BYTE> pbHash(dwHashSize);
    if (!CryptCATAdminCalcHashFromFileHandle(hFile, &dwHashSize, pbHash.data(), 0)) {
        CloseHandle(hFile);
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return false;
    }
    CloseHandle(hFile);

    CATALOG_INFO catInfo = { sizeof(CATALOG_INFO) };
    HANDLE hCatInfo = CryptCATAdminEnumCatalogFromHash(hCatAdmin, pbHash.data(), dwHashSize, 0, nullptr);
    bool isCatalogSigned = false;

    while (hCatInfo && CryptCATCatalogInfoFromContext(hCatInfo, &catInfo, 0)) {
        WINTRUST_CATALOG_INFO wtc = { sizeof(WINTRUST_CATALOG_INFO) };
        wtc.pcwszCatalogFilePath = catInfo.wszCatalogFile;
        wtc.pbCalculatedFileHash = pbHash.data();
        wtc.cbCalculatedFileHash = dwHashSize;
        wtc.pcwszMemberFilePath = filePath.c_str();

        WINTRUST_DATA wtd = { sizeof(WINTRUST_DATA) };
        wtd.dwUnionChoice = WTD_CHOICE_CATALOG;
        wtd.pCatalog = &wtc;
        wtd.dwUIChoice = WTD_UI_NONE;
        wtd.fdwRevocationChecks = WTD_REVOKE_NONE;
        wtd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        wtd.dwStateAction = WTD_STATEACTION_VERIFY;

        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        LONG res = WinVerifyTrust(nullptr, &action, &wtd);

        wtd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &wtd);

        if (res == ERROR_SUCCESS) {
            isCatalogSigned = true;
            break;
        }
        hCatInfo = CryptCATAdminEnumCatalogFromHash(hCatAdmin, pbHash.data(), dwHashSize, 0, &hCatInfo);
    }

    if (hCatInfo)
        CryptCATAdminReleaseCatalogContext(hCatAdmin, hCatInfo, 0);

    CryptCATAdminReleaseContext(hCatAdmin, 0);
    return isCatalogSigned;
}

wchar_t GetWindowsDriveLetter()
{
    static wchar_t driveLetter = 0;
    if (!driveLetter) {
        wchar_t windowsPath[MAX_PATH] = { 0 };
        if (GetWindowsDirectoryW(windowsPath, MAX_PATH))
            driveLetter = windowsPath[0];
    }
    return driveLetter;
}

wchar_t ToUpperFast(wchar_t c)
{
    return (c >= L'a' && c <= L'z') ? c - 32 : c;
}

bool IsPathForcedSigned(const std::wstring& rawPath)
{
    wchar_t winDrive = GetWindowsDriveLetter();
    if (winDrive == 0)
        winDrive = L'C';

    std::wstring norm;
    norm.reserve(rawPath.size());

    size_t start = 0;
    if (rawPath.size() >= 2 && rawPath[1] == L':' && ToUpperFast(rawPath[0]) == winDrive)
        start = 2;

    for (size_t i = start; i < rawPath.size(); ++i) {
        wchar_t ch = rawPath[i];
        if (ch == L'/')
            ch = L'\\';
        norm.push_back(ToUpperFast(ch));
    }

    return g_forcedSignedPaths.find(norm) != g_forcedSignedPaths.end();
}

SignatureStatus GetSignatureStatus(const std::wstring& path)
{
    {
        std::shared_lock readLock(g_signatureMutex);
        if (auto it = g_signatureCache.find(path); it != g_signatureCache.end())
            return it->second;
    }

    if (IsPathForcedSigned(path))
        return SignatureStatus::Signed;

    static std::wstring exePath;
    if (exePath.empty()) {
        wchar_t buffer[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(nullptr, buffer, MAX_PATH))
            exePath = buffer;
    }
    if (_wcsicmp(path.c_str(), exePath.c_str()) == 0)
        return SignatureStatus::Signed;

    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::unique_lock writeLock(g_signatureMutex);
        g_signatureCache[path] = SignatureStatus::NotFound;
        return SignatureStatus::NotFound;
    }

    SignatureStatus status = SignatureStatus::Signed;
    try {
        if (IsPEFile(path)) {
            auto signingCertOpt = GetSignerCertificate(path);
            if (signingCertOpt.has_value()) {
                PCCERT_CONTEXT signingCert = *signingCertOpt;

                char subjectName[256];
                CertNameToStrA(signingCert->dwCertEncodingType, &signingCert->pCertInfo->Subject, CERT_X500_NAME_STR, subjectName, sizeof(subjectName));
                std::string_view subject(subjectName);
                std::string lowerSubject(subject);
                std::transform(lowerSubject.begin(), lowerSubject.end(), lowerSubject.begin(), ::tolower);
                static const std::string_view cheats[] = { "manthe industries, llc", "slinkware", "amstion limited", "newfakeco", "faked signatures inc" };
                bool isCheat = false;
                for (auto c : cheats) {
                    if (lowerSubject.find(c) != std::string::npos) {
                        isCheat = true;
                        break;
                    }
                }

                if (isCheat) {
                    status = SignatureStatus::Cheat;
                }
                else {
                    std::string headerHash = ComputeFileHeaderHash(path);
                    if (!headerHash.empty()) {
                        std::unique_lock winTrustLock(g_winTrustMutex);
                        if (auto it = g_winTrustCache.find(headerHash); it != g_winTrustCache.end()) {
                            status = it->second;
                        }
                        else {
                            WINTRUST_FILE_INFO fileInfo = { sizeof(WINTRUST_FILE_INFO) };
                            fileInfo.pcwszFilePath = path.c_str();

                            WINTRUST_DATA winTrustData = { sizeof(WINTRUST_DATA) };
                            winTrustData.dwUIChoice = WTD_UI_NONE;
                            winTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
                            winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
                            winTrustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
                            winTrustData.dwStateAction = WTD_STATEACTION_VERIFY;
                            winTrustData.pFile = &fileInfo;

                            GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
                            LONG res = WinVerifyTrust(nullptr, &action, &winTrustData);

                            winTrustData.dwStateAction = WTD_STATEACTION_CLOSE;
                            WinVerifyTrust(nullptr, &action, &winTrustData);

                            if (res == ERROR_SUCCESS) {
                                status = SignatureStatus::Signed;
                            }
                            else {
                                status = SignatureStatus::Fake;
                            }

                            g_winTrustCache[headerHash] = status;
                        }
                    }
                    else {
                        status = SignatureStatus::Fake;
                    }
                }

                CertFreeCertificateContext(signingCert);
            }
            else {
                if (VerifyFileViaCatalog(path)) {
                    status = SignatureStatus::Signed;
                }
                else {
                    status = SignatureStatus::Unsigned;
                }
            }
        }
        else {
            status = SignatureStatus::Signed;
        }
    }
    catch (...) {
        status = SignatureStatus::Signed;
    }

    {
        std::unique_lock writeLock(g_signatureMutex);
        g_signatureCache[path] = status;
    }

    return status;
}

std::future<SignatureStatus> GetSignatureStatusAsync(const std::wstring& path) {
    return std::async(std::launch::async, GetSignatureStatus, path);
}