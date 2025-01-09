#include "singletons/Toasts.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "util/StreamLink.hpp"
#include "widgets/helper/CommonTexts.hpp"

#ifdef Q_OS_WIN
#    include <wintoastlib.h>
#endif

#include <KNotification>

#include <QDesktopServices>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStringBuilder>
#include <QUrl>

#include <utility>

namespace {

using namespace chatterino;
using namespace literals;

QString avatarFilePath(const QString &channelName)
{
    // TODO: cleanup channel (to be used as a file) and use combinePath
    return getApp()->getPaths().twitchProfileAvatars % '/' % channelName %
           u".png";
}

bool hasAvatarForChannel(const QString &channelName)
{
    QFileInfo avatarFile(avatarFilePath(channelName));
    return avatarFile.exists() && avatarFile.isFile();
}

/// A job that downlaods a twitch avatar and saves it to a file
class AvatarDownloader : public QObject
{
    Q_OBJECT
public:
    AvatarDownloader(const QString &avatarURL, const QString &channelName);

private:
    QNetworkAccessManager manager_;
    QFile file_;
    QNetworkReply *reply_{};

signals:
    void downloadComplete();
};

}  // namespace

namespace chatterino {

Toasts::~Toasts() {
}

bool Toasts::isEnabled()
{
    return
        // WinToast::isCompatible() &&
        getSettings()->notificationToast &&
           !(getApp()->getStreamerMode()->isEnabled() &&
             getSettings()->streamerModeSuppressLiveNotifications);
}

QString Toasts::findStringFromReaction(const ToastReaction &reaction)
{
    switch (reaction)
    {
        case ToastReaction::OpenInBrowser:
            return OPEN_IN_BROWSER;
        case ToastReaction::OpenInPlayer:
            return OPEN_PLAYER_IN_BROWSER;
        case ToastReaction::OpenInStreamlink:
            return OPEN_IN_STREAMLINK;
        case ToastReaction::DontOpen:
        default:
            return DONT_OPEN;
    }
}

QString Toasts::findStringFromReaction(
    const pajlada::Settings::Setting<int> &reaction)
{
    static_assert(std::is_same_v<std::underlying_type_t<ToastReaction>, int>);
    int value = reaction;
    return Toasts::findStringFromReaction(static_cast<ToastReaction>(value));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void Toasts::sendChannelNotification(const QString &channelName,
                                     const QString &channelTitle, Platform p)
{
    qCDebug(chatterinoNotification) << "about to send channel notification";
    auto sendChannelNotification = [this, channelName, channelTitle, p] {
        this->sendNotification(channelName, channelTitle, p);
    };
    // Fetch user profile avatar
    if (p == Platform::Twitch)
    {
        if (hasAvatarForChannel(channelName))
        {
            sendChannelNotification();
        }
        else
        {
            getHelix()->getUserByName(
                channelName,
                [channelName, sendChannelNotification](const auto &user) {
                    // gets deleted when finished
                    auto *downloader =
                        new AvatarDownloader(user.profileImageUrl, channelName);
                    QObject::connect(downloader,
                                     &AvatarDownloader::downloadComplete,
                                     sendChannelNotification);
                },
                [] {
                    // on failure
                });
        }
    }
}

void Toasts::sendNotification(const QString &channelName,
                                     const QString &channelTitle, Platform p)
{
    qCDebug(chatterinoNotification) << "REALLY about to send channel notification";
    KNotification *notification = new KNotification(QStringLiteral("live"));

    QString str = channelName % u" is live!";

    notification->setTitle(str);
    if (static_cast<ToastReaction>(getSettings()->openFromToast.getValue()) !=
        ToastReaction::DontOpen)
    {
        qCDebug(chatterinoNotification) << "Toast Reaction is not DontOpen";
        QString mode =
            Toasts::findStringFromReaction(getSettings()->openFromToast);
        mode = mode.toLower();

        notification->setText(
            QString::fromStdWString(u"%1 \nClick to %2"_s.arg(channelTitle).arg(mode).toStdWString())
            );
    }

    QString avatarPath;
    if (p == Platform::Twitch)
    {
        avatarPath = avatarFilePath(channelName);
    }
    notification->setPixmap(avatarPath);
    KNotificationAction* action_p = notification->addDefaultAction("Open");

    auto conn = QObject::connect(action_p, &KNotificationAction::activated,
                     // qobject_cast<KNotificationAction *>(this),
                     [channelName, p] {
        qCDebug(chatterinoNotification) << "Action triggered";
        auto toastReaction =
            static_cast<ToastReaction>(getSettings()->openFromToast.getValue());

        switch (toastReaction)
        {
            case ToastReaction::OpenInBrowser:
                if (p == Platform::Twitch)
                {
                    QDesktopServices::openUrl(
                        QUrl(u"https://www.twitch.tv/" % channelName));
                }
                break;
            case ToastReaction::OpenInPlayer:
                if (p == Platform::Twitch)
                {
                    QDesktopServices::openUrl(
                        QUrl(TWITCH_PLAYER_URL.arg(channelName)));
                }
                break;
            case ToastReaction::OpenInStreamlink: {
                openStreamlinkForChannel(channelName);
                break;
            }
            case ToastReaction::DontOpen:
                // nothing should happen
                break;
        }
    });

    if (!conn) {
        qCDebug(chatterinoNotification) << "CONNECTION FAILED";
    }

    if (!this) {
        qCDebug(chatterinoNotification) << "NULL this";
    }
    if (!qobject_cast<KNotificationAction *>(this)) {
        qCDebug(chatterinoNotification) << "NULL CAST";
    }

    qCDebug(chatterinoNotification) << "About to send event";
    notification->sendEvent();
}

}  // namespace chatterino

namespace {

AvatarDownloader::AvatarDownloader(const QString &avatarURL,
                                   const QString &channelName)
    : file_(avatarFilePath(channelName))
{
    if (!this->file_.open(QFile::WriteOnly | QFile::Truncate))
    {
        qCWarning(chatterinoNotification)
            << "Failed to open avatar file" << this->file_.errorString();
    }

    this->reply_ = this->manager_.get(QNetworkRequest(avatarURL));

    connect(this->reply_, &QNetworkReply::readyRead, this, [this] {
        this->file_.write(this->reply_->readAll());
    });
    connect(this->reply_, &QNetworkReply::finished, this, [this] {
        if (this->reply_->error() != QNetworkReply::NoError)
        {
            qCWarning(chatterinoNotification)
                << "Failed to download avatar" << this->reply_->errorString();
        }

        if (this->file_.isOpen())
        {
            this->file_.close();
        }
        emit downloadComplete();
        this->deleteLater();
    });
}

#include "Toasts.moc"

}  // namespace
