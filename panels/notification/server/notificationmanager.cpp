// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notificationmanager.h"
#include "notificationsetting.h"
#include "notifyentity.h"
#include "dbaccessor.h"

#include <QDateTime>
#include <DDesktopServices>
#include <DSGApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>

DCORE_USE_NAMESPACE

namespace notification {

static const uint NoReplacesId = 0;
static const QString NotificationsDBusService = "org.freedesktop.Notifications";
static const QString NotificationsDBusPath = "/org/freedesktop/Notifications";
static const QString DDENotifyDBusServer = "org.deepin.dde.Notification1";
static const QString DDENotifyDBusPath = "/org/deepin/dde/Notification1";
static const QString SessionDBusService = "org.deepin.dde.SessionManager1";
static const QString SessionDaemonDBusPath = "/org/deepin/dde/SessionManager1";
static const QStringList IgnoreList= {
        "dde-control-center"
};

NotificationManager::NotificationManager(QObject *parent)
    : QObject(parent)
    , m_persistence(new DBAccessor())
    , m_notificationSetting(new NotificationSetting(this))
    , m_userSessionManager(new UserSessionManager(SessionDBusService, SessionDaemonDBusPath, QDBusConnection::sessionBus(), this))
{

}

NotificationManager::~NotificationManager()
{
    delete m_persistence;
}

bool NotificationManager::registerDbusService()
{
    QDBusConnection connection = QDBusConnection::sessionBus();
    connection.interface()->registerService(NotificationsDBusService,
                                            QDBusConnectionInterface::ReplaceExistingService,
                                            QDBusConnectionInterface::AllowReplacement);
    if (!connection.registerObject(NotificationsDBusPath, this)) {
        return false;
    }

    QDBusConnection ddeNotifyConnect = QDBusConnection::sessionBus();
    ddeNotifyConnect.interface()->registerService(DDENotifyDBusServer,
                                                  QDBusConnectionInterface::ReplaceExistingService,
                                                  QDBusConnectionInterface::AllowReplacement);
    if (!ddeNotifyConnect.registerObject(DDENotifyDBusPath, this)) {
        return false;
    }

    return true;
}

void NotificationManager::actionInvoked(qint64 id, uint bubbleId, const QString &actionKey)
{
    m_persistence->updateEntityProcessedType(id, NotifyEntity::processedValue());

    Q_EMIT ActionInvoked(bubbleId, actionKey);
    Q_EMIT NotificationClosed(bubbleId, NotificationManager::Closed);
}

void NotificationManager::notificationClosed(qint64 id, uint bubbleId, uint reason)
{
    m_persistence->updateEntityProcessedType(id, NotifyEntity::processedValue());

    Q_EMIT NotificationClosed(bubbleId, reason);
}

void NotificationManager::notificationReplaced(qint64 id)
{
    m_persistence->removeEntity(id);
}

QStringList NotificationManager::GetCapabilities()
{
    QStringList result;
    result << "action-icons" << "actions" << "body" << "body-hyperlinks" << "body-markup" << "body-images" << "persistence";

    return result;
}

uint NotificationManager::Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary,
                                 const QString &body, const QStringList &actions, const QVariantMap &hints,
                                 int expireTimeout)
{
    qInfo() << "Notify" << appName << replacesId << appIcon << summary << body << actions << hints << expireTimeout;
    if (calledFromDBus() && m_notificationSetting->getSystemSetting(NotificationSetting::CloseNotification).toBool()) {
        return 0;
    }

    QString appId;
    if (calledFromDBus()) {
        QDBusReply<uint> reply = connection().interface()->servicePid(message().service());
        appId = DSGApplication::getId(reply.value());
    }

    if (appId.isEmpty())
        appId = appName;

    bool enableAppNotification = m_notificationSetting->getAppSetting(appId, NotificationSetting::EnableNotification).toBool();
    if (!enableAppNotification && !IgnoreList.contains(appId)) {
        return 0;
    }

    QString strBody = body;
    strBody.replace(QLatin1String("\\\\"), QLatin1String("\\"), Qt::CaseInsensitive);

    QString strIcon = appIcon;
    if (strIcon.isEmpty())
        strIcon = m_notificationSetting->getAppSetting(appId, NotificationSetting::AppIcon).toString();
    NotifyEntity entity(appName, replacesId, strIcon, summary, strBody, actions, hints, expireTimeout);

    bool enablePreview = true, lockScreenShow = true;
    bool dndMode = isDoNotDisturb();
    bool systemNotification = IgnoreList.contains(appName);
    bool lockScreen = m_userSessionManager->locked();

    if (!systemNotification) {
        enablePreview = m_notificationSetting->getAppSetting(appId, NotificationSetting::EnablePreview).toBool();
        lockScreenShow = m_notificationSetting->getAppSetting(appId, NotificationSetting::LockScreenShowNotification).toBool();
    }

    entity.setEnablePreview(enablePreview);

    tryPlayNotificationSound(appName, dndMode, actions, hints);

    // new one
    if (replacesId == NoReplacesId) {
        entity.setBubbleId(++m_replacesCount);
        tryRecordEntity(appId, entity);

        if (systemNotification || (!dndMode && enableAppNotification && (lockScreenShow || !lockScreen))) {
            Q_EMIT needShowEntity(entity.toVariantMap());
        }
    } else { // maybe replace one
        entity.setBubbleId(replacesId);
        tryRecordEntity(appId, entity);

        Q_EMIT needShowEntity(entity.toVariantMap());
    }

    // If replaces_id is 0, the return value is a UINT32 that represent the notification.
    // If replaces_id is not 0, the returned value is the same value as replaces_id.
    return replacesId == 0 ? entity.bubbleId() : replacesId;
}
void NotificationManager::CloseNotification(uint id)
{
    // TODO If the notification no longer exists, an empty D-BUS Error message is sent back.
    Q_EMIT needCloseEntity(id);

    Q_EMIT NotificationClosed(id, NotificationManager::Closed);
}

void NotificationManager::GetServerInformation(QString &name, QString &vendor, QString &version, QString &specVersion)
{
    name = QString("DeepinNotifications");
    vendor = QString("Deepin");
    version = QString("3.0");

    specVersion = QString("1.2");
}

QStringList NotificationManager::GetAppList()
{
    return QStringList();
}

QDBusVariant NotificationManager::GetAppInfo(const QString &appId, uint configItem)
{
    return QDBusVariant();
}

void NotificationManager::SetAppInfo(const QString &appId, uint configItem, const QDBusVariant &value)
{

}

QString NotificationManager::GetAppSetting(const QString &appName)
{
    return QString();
}

void NotificationManager::SetAppSetting(const QString &settings)
{

}

void NotificationManager::SetSystemInfo(uint configItem, const QDBusVariant &value)
{

}

QDBusVariant NotificationManager::GetSystemInfo(uint configItem)
{
    return QDBusVariant();
}

bool NotificationManager::isDoNotDisturb() const
{
    if (!m_notificationSetting->getSystemSetting(NotificationSetting::DNDMode).toBool())
        return false;

    // 未点击按钮  任何时候都勿扰模式
    if (!m_notificationSetting->getSystemSetting(NotificationSetting::OpenByTimeInterval).toBool() &&
        !m_notificationSetting->getSystemSetting(NotificationSetting::LockScreenOpenDNDMode).toBool()) {
        return true;
    }

    bool lockScreen = m_userSessionManager->locked();
    // 点击锁屏时 并且 锁屏状态 任何时候都勿扰模式
    if (m_notificationSetting->getSystemSetting(NotificationSetting::LockScreenOpenDNDMode).toBool() && lockScreen)
        return true;

    QTime currentTime = QTime::fromString(QDateTime::currentDateTime().toString("hh:mm"));
    QTime startTime = QTime::fromString(m_notificationSetting->getSystemSetting(NotificationSetting::StartTime).toString());
    QTime endTime = QTime::fromString(m_notificationSetting->getSystemSetting(NotificationSetting::EndTime).toString());

    bool dndMode = false;
    if (startTime < endTime) {
        dndMode = startTime <= currentTime && endTime >= currentTime;
    } else if (startTime > endTime) {
        dndMode = startTime <= currentTime || endTime >= currentTime;
    } else {
        dndMode = true;
    }

    if (dndMode && m_notificationSetting->getSystemSetting(NotificationSetting::OpenByTimeInterval).toBool()) {
        return dndMode;
    } else {
        return false;
    }
}

void NotificationManager::tryPlayNotificationSound(const QString &appName, bool dndMode, const QStringList &actions,
                                                   const QVariantMap &hints) const
{
    auto playSoundTip = [] {
        Dtk::Gui::DDesktopServices::playSystemSoundEffect(Dtk::Gui::DDesktopServices::SSE_Notifications);
    };

    bool playSound = true;
    bool systemNotification = IgnoreList.contains(appName);
    if (!systemNotification) {
        playSound = m_notificationSetting->getAppSetting(appName, NotificationSetting::EnableSound).toBool();
    }

    if (playSound && !dndMode) {
        QString action;
        //接收蓝牙文件时，只在发送完成后才有提示音,"cancel"表示正在发送文件
        if (actions.contains("cancel") && hints.contains("x-deepin-action-_view")) {
            action = hints["x-deepin-action-_view"].toString();
            if (action.contains("xdg-open")) {
                playSoundTip();
                return;
            }
        } else {
            playSoundTip();
        }
    }

    if (systemNotification && dndMode) {
        playSoundTip();
    }
}

void NotificationManager::tryRecordEntity(const QString &appId, NotifyEntity &entity)
{
    bool showInNotificationCenter = m_notificationSetting->getAppSetting(appId, NotificationSetting::ShowInNotificationCenter).toBool();
    if (!showInNotificationCenter) {
        return;
    }

    // "cancel"表示正在发送蓝牙文件,不需要发送到通知中心
    if (entity.body().contains("%") && entity.actions().contains("cancel")) {
        return;
    }

    qint64 id = m_persistence->addEntity(entity);
    entity.setId(id);
}

} // notification