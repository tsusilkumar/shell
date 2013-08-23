/****************************************************************************
 * This file is part of Hawaii Shell.
 *
 * Copyright (C) 2012-2013 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
 *
 * Author(s):
 *    Pier Luigi Fiorini
 *
 * $BEGIN_LICENSE:LGPL2.1+$
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * $END_LICENSE$
 ***************************************************************************/

#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>

#include <qpa/qplatformnativeinterface.h>

#include "applicationiconprovider.h"
#include "cmakedirs.h"
#include "registration.h"
#include "desktopshell.h"
#include "notificationwindow.h"
#include "waylandintegration.h"
#include "shellui.h"
#include "shellwindow.h"
#include "window.h"
#include "workspace.h"
#include "keybinding.h"
#include "servicefactory.h"

Q_GLOBAL_STATIC(DesktopShell, s_desktopShell)

DesktopShell::DesktopShell()
    : QObject()
{
    // Start counting how much time we need to start up :)
    m_elapsedTimer.start();

    // We need windows with alpha buffer
    QQuickWindow::setDefaultAlphaBuffer(true);

    // Create QML engine
    m_engine = new QQmlEngine(this);
    m_engine->rootContext()->setContextProperty("Shell", this);

    // Register image provider
    m_engine->addImageProvider("appicon", new ApplicationIconProvider);

    // Register QML types and factories
    registerQmlTypes();
    registerFactories();

    // Platform native interface
    QPlatformNativeInterface *native =
            QGuiApplication::platformNativeInterface();
    Q_ASSERT(native);

    // Get Wayland display
    m_display = static_cast<struct wl_display *>(
                native->nativeResourceForIntegration("display"));
    Q_ASSERT(m_display);

    // Display file descriptor
    m_fd = wl_display_get_fd(m_display);
    Q_ASSERT(m_fd > -1);
    qDebug() << "Wayland display socket:" << m_fd;

    // Wayland registry
    m_registry = wl_display_get_registry(m_display);
    Q_ASSERT(m_registry);

    // Wayland integration
    WaylandIntegration *integration = WaylandIntegration::instance();
    wl_registry_add_listener(m_registry, &WaylandIntegration::registryListener,
                             integration);
}

DesktopShell::~DesktopShell()
{
    // Delete workspaces and shell windows
    qDeleteAll(m_workspaces);
    qDeleteAll(m_shellWindows);
    qDeleteAll(m_services);
    delete m_engine;

    // Unbind interfaces
    WaylandIntegration *integration = WaylandIntegration::instance();
    wl_notification_daemon_destroy(integration->notification);
    hawaii_desktop_shell_destroy(integration->shell);
}

DesktopShell *DesktopShell::instance()
{
    return s_desktopShell();
}

void DesktopShell::create()
{
    // Create shell windows
    foreach (QScreen *screen, QGuiApplication::screens()) {
        qDebug() << "--- Screen" << screen->name() << screen->geometry();

        ShellUi *ui = new ShellUi(m_engine, screen, this);
        m_shellWindows.append(ui);
    }

    // Wait until all user interface elements for all screens are ready
    while (QCoreApplication::hasPendingEvents())
        QCoreApplication::processEvents();

    // Add the first workspace
    // TODO: Add as many workspaces as specified by the settings
    addWorkspace();
    addWorkspace();
    addWorkspace();
    addWorkspace();

    // Process workspaces events
    while (QCoreApplication::hasPendingEvents())
        QCoreApplication::processEvents();

    // Shell user interface is ready, tell the compositor to fade in
    ready();
}

void DesktopShell::ready()
{
    WaylandIntegration *integration = WaylandIntegration::instance();
    hawaii_desktop_shell_desktop_ready(integration->shell);
    qDebug() << "Shell is now ready and took" << m_elapsedTimer.elapsed() << "ms";
}

QObject *DesktopShell::service(const QString &name)
{
    // Get an already created service
    QObject *service = m_services.value(name);
    if (service)
        return service;

    // If we can't find it just create it
    service = ServiceFactory::createService(name);
    m_services[name] = service;
    return service;
}

KeyBinding *DesktopShell::addKeyBinding(quint32 key, quint32 modifiers)
{
    KeyBinding *keyBinding = new KeyBinding(key, modifiers, this);
    m_keyBindings.append(keyBinding);
    return keyBinding;
}

QQmlListProperty<Window> DesktopShell::windows()
{
    return QQmlListProperty<Window>(this, 0, windowsCount, windowAt);
}

QQmlListProperty<Workspace> DesktopShell::workspaces()
{
    return QQmlListProperty<Workspace>(this, 0, workspacesCount, workspaceAt);
}

void DesktopShell::minimizeWindows()
{
    WaylandIntegration *integration = WaylandIntegration::instance();
    hawaii_desktop_shell_minimize_windows(integration->shell);
}

void DesktopShell::restoreWindows()
{
    WaylandIntegration *integration = WaylandIntegration::instance();
    hawaii_desktop_shell_restore_windows(integration->shell);
}

void DesktopShell::addWorkspace()
{
    WaylandIntegration *integration = WaylandIntegration::instance();
    hawaii_desktop_shell_add_workspace(integration->shell);
}

void DesktopShell::removeWorkspace(int num)
{
    Workspace *workspace = m_workspaces.takeAt(num);
    if (workspace) {
        Q_EMIT workspaceRemoved(num);
        delete workspace;
    }
}

void DesktopShell::appendWindow(Window *window)
{
    m_windows.append(window);
    connect(window, SIGNAL(unmapped(Window*)),
            this, SLOT(windowUnmapped(Window*)));
    Q_EMIT windowsChanged();
}

void DesktopShell::appendWorkspace(Workspace *workspace)
{
    m_workspaces.append(workspace);
    Q_EMIT workspaceAdded(m_workspaces.indexOf(workspace));
    Q_EMIT workspacesChanged();
}

int DesktopShell::windowsCount(QQmlListProperty<Window> *p)
{
    DesktopShell *shell = static_cast<DesktopShell *>(p->object);
    return shell->m_windows.size();
}

Window *DesktopShell::windowAt(QQmlListProperty<Window> *p, int index)
{
    DesktopShell *shell = static_cast<DesktopShell *>(p->object);
    return shell->m_windows.at(index);
}

int DesktopShell::workspacesCount(QQmlListProperty<Workspace> *p)
{
    DesktopShell *shell = static_cast<DesktopShell *>(p->object);
    return shell->m_workspaces.size();
}

Workspace *DesktopShell::workspaceAt(QQmlListProperty<Workspace> *p, int index)
{
    DesktopShell *shell = static_cast<DesktopShell *>(p->object);
    return shell->m_workspaces.at(index);
}

void DesktopShell::windowUnmapped(Window *window)
{
    m_windows.removeOne(window);
    Q_EMIT windowsChanged();
}

#include "moc_desktopshell.cpp"
