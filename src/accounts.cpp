// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/accounts.hpp"
#include <QJsonDocument>
#ifdef Q_OS_MACOS
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#else
#include <QProcess>
#endif

namespace bilive {
namespace {
enum class Op { Read, Write, Remove };

Error unavailable()
{
    return {Error::Kind::Input, 0, QStringLiteral("系统凭据库不可用或拒绝了操作")};
}

bool validUid(const QString &uid)
{
    bool ok = false;
    const auto n = uid.toULongLong(&ok);
    return ok && n > 0 && uid == QString::number(n);
}

Result<QByteArray> vault(const QString &uid, Op op, const QByteArray &bytes = {})
{
    if (!validUid(uid))
        return unavailable();
#ifdef Q_OS_MACOS
    auto dict = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks);
    const auto accountId = uid.toLatin1();
    auto name = CFStringCreateWithCString(nullptr, accountId.constData(), kCFStringEncodingASCII);
    CFDictionarySetValue(dict, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(dict, kSecAttrService, CFSTR("local.bilive.obs"));
    CFDictionarySetValue(dict, kSecAttrAccount, name);
    CFRelease(name);
    OSStatus status;
    QByteArray result;
    if (op == Op::Write) {
        auto value = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(bytes.constData()), bytes.size());
        const void *keys[] = {kSecValueData};
        const void *values[] = {value};
        auto updates = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
        status = SecItemUpdate(dict, updates);
        if (status == errSecItemNotFound) {
            CFDictionarySetValue(dict, kSecValueData, value);
            status = SecItemAdd(dict, nullptr);
        }
        CFRelease(updates);
        CFRelease(value);
    } else if (op == Op::Read) {
        CFDictionarySetValue(dict, kSecReturnData, kCFBooleanTrue);
        CFTypeRef value = nullptr;
        status = SecItemCopyMatching(dict, &value);
        if (status == errSecSuccess && value) {
            const auto data = static_cast<CFDataRef>(value);
            result = QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), CFDataGetLength(data));
        }
        if (value)
            CFRelease(value);
    } else {
        status = SecItemDelete(dict);
        if (status == errSecItemNotFound)
            status = errSecSuccess;
    }
    CFRelease(dict);
    if (status != errSecSuccess)
        return unavailable();
    return result;
#elif defined(Q_OS_WIN)
    auto key = (QStringLiteral("local.bilive.obs/") + uid).toStdWString();
    if (op == Op::Write) {
        if (bytes.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
            return unavailable();
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = key.data();
        cred.CredentialBlobSize = static_cast<DWORD>(bytes.size());
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(bytes.constData()));
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&cred, 0))
            return unavailable();
        return QByteArray{};
    }
    if (op == Op::Remove) {
        if (!CredDeleteW(key.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND)
            return unavailable();
        return QByteArray{};
    }
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(key.c_str(), CRED_TYPE_GENERIC, 0, &cred))
        return unavailable();
    auto result = QByteArray(reinterpret_cast<const char *>(cred->CredentialBlob), cred->CredentialBlobSize);
    CredFree(cred);
    return result;
#else
    QProcess process;
    QStringList args;
    if (op == Op::Write)
        args = {QStringLiteral("store"), QStringLiteral("--label=bilive OBS")};
    else
        args = {op == Op::Read ? QStringLiteral("lookup") : QStringLiteral("clear")};
    args << QStringLiteral("application") << QStringLiteral("local.bilive.obs")
         << QStringLiteral("account") << uid;
    process.start(QStringLiteral("secret-tool"), args);
    if (!process.waitForStarted(700))
        return unavailable();
    if (op == Op::Write)
        process.write(bytes);
    process.closeWriteChannel();
    if (!process.waitForFinished(1800)) {
        process.kill();
        process.waitForFinished();
        return unavailable();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return unavailable();
    return process.readAllStandardOutput().trimmed();
#endif
}
} // namespace

QStringList AccountStore::accounts() const
{
    auto list = settings_.value(QStringLiteral("accounts/uids")).toStringList();
    list.removeIf([](const auto &uid) { return !validUid(uid); });
    list.removeDuplicates();
    return list;
}

Result<Credentials> AccountStore::load(const QString &uid) const
{
    auto reply = vault(uid, Op::Read);
    if (const auto *error = std::get_if<Error>(&reply))
        return *error;
    const auto json = QJsonDocument::fromJson(std::get<QByteArray>(reply)).object();
    Credentials credentials;
    for (auto it = json.begin(); it != json.end(); ++it)
        credentials.cookies.insert(it.key().toLatin1(), it.value().toString().toLatin1());
    if (!credentials.valid() || credentials.uid() != uid)
        return unavailable();
    return credentials;
}

Result<bool> AccountStore::save(const Credentials &credentials)
{
    if (!credentials.valid())
        return unavailable();
    QJsonObject json;
    for (auto it = credentials.cookies.begin(); it != credentials.cookies.end(); ++it)
        json.insert(QString::fromLatin1(it.key()), QString::fromLatin1(it.value()));
    const auto uid = credentials.uid();
    auto reply = vault(uid, Op::Write, QJsonDocument(json).toJson(QJsonDocument::Compact));
    if (const auto *error = std::get_if<Error>(&reply))
        return *error;
    auto list = accounts();
    if (!list.contains(uid)) {
        list.append(uid);
        settings_.setValue(QStringLiteral("accounts/uids"), list);
        settings_.sync();
    }
    return true;
}

Result<bool> AccountStore::remove(const QString &uid)
{
    const auto reply = vault(uid, Op::Remove);
    if (const auto *error = std::get_if<Error>(&reply))
        return *error;
    auto list = accounts();
    list.removeAll(uid);
    settings_.setValue(QStringLiteral("accounts/uids"), list);
    settings_.sync();
    return true;
}
}
