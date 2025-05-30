/***************************************************************************
 *   Copyright (C) 2006 by Vladimir Kuznetsov                              *
 *   vovanec@gmail.com                                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include <utility>

#include <QDockWidget>
#include <QScreen>
#include <QToolButton>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <functional>
#include <QGuiApplication>
#include <QActionGroup>

#ifdef HAVE_QDBUS
#include <QtDBus/QtDBus>
#include "windowadaptor.h"
#endif

#include "terminalconfig.h"
#include "mainwindow.h"
#include "tabwidget.h"
#include "termwidgetholder.h"
#include "config.h"
#include "properties.h"
#include "propertiesdialog.h"
#include "bookmarkswidget.h"
#include "qterminalapp.h"
#include "dbusaddressable.h"

#include <LayerShellQt/Shell>
#include <LayerShellQt/Window>

typedef std::function<bool(MainWindow&, QAction *)> checkfn;
Q_DECLARE_METATYPE(checkfn)

MainWindow::MainWindow(TerminalConfig &cfg,
                       bool dropMode,
                       QWidget * parent,
                       Qt::WindowFlags f)
    : QMainWindow(parent,f),
      DBusAddressable(QStringLiteral("/windows")),
      tabPosition(nullptr),
      scrollBarPosition(nullptr),
      keyboardCursorShape(nullptr),
      tabPosMenu(nullptr),
      scrollPosMenu(nullptr),
      keyboardCursorShapeMenu(nullptr),
      settingOwner(nullptr),
      presetsMenu(nullptr),
      m_config(cfg),
      m_dropLockButton(nullptr),
      m_dropMode(dropMode),
      m_layerWindow(nullptr)
{
#ifdef HAVE_QDBUS
    registerAdapter<WindowAdaptor, MainWindow>(this);
#endif
    QTerminalApp::Instance()->addWindow(this);
    // We want terminal translucency...
    setAttribute(Qt::WA_TranslucentBackground);
    // ... but neither a fully transparent nor a flat menubar
    // with styles that have translucency and/or gradient.
    setAttribute(Qt::WA_NoSystemBackground, false);
    setAttribute(Qt::WA_DeleteOnClose);

    setupUi(this);

    // Allow insane small sizes - reason:
    // https://github.com/lxqt/qterminal/issues/181 - Minimum size
    // https://github.com/lxqt/qterminal/issues/263 - Decrease minimal height
    QFontMetrics metrics(Properties::Instance()->font);
    int spaceWidth = metrics.horizontalAdvance(QChar(QChar::Space));
    setMinimumSize(QSize(10 * spaceWidth, metrics.height()));

    m_bookmarksDock = new QDockWidget(tr("Bookmarks"), this);
    m_bookmarksDock->setObjectName(QStringLiteral("BookmarksDockWidget"));
    m_bookmarksDock->setAutoFillBackground(true);
    BookmarksWidget *bookmarksWidget = new BookmarksWidget(m_bookmarksDock);
    bookmarksWidget->setAutoFillBackground(true);
    m_bookmarksDock->setWidget(bookmarksWidget);
    addDockWidget(Qt::LeftDockWidgetArea, m_bookmarksDock);
    connect(bookmarksWidget, &BookmarksWidget::callCommand,
            this, &MainWindow::bookmarksWidget_callCommand);

    connect(m_bookmarksDock, &QDockWidget::visibilityChanged,
            this, &MainWindow::bookmarksDock_visibilityChanged);

    connect(actAbout, &QAction::triggered, this, &MainWindow::actAbout_triggered);
    connect(actAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
    connect(&m_dropShortcut, &QxtGlobalShortcut::activated, this, &MainWindow::showHide);

    setContentsMargins(0, 0, 0, 0);
    if (m_dropMode) {
        this->enableDropMode();
    }
    else {
        if (Properties::Instance()->saveSizeOnExit) {
            if (Properties::Instance()->mainWindowSize.isValid())
                resize(Properties::Instance()->mainWindowSize);
        }
        else if (Properties::Instance()->fixedWindowSize.isValid()) {
            resize(Properties::Instance()->fixedWindowSize);
        }
        if (Properties::Instance()->savePosOnExit && !Properties::Instance()->mainWindowPosition.isNull()
            && QGuiApplication::platformName() != QStringLiteral("wayland")
            ) {
            move(Properties::Instance()->mainWindowPosition);
        }
        if (Properties::Instance()->saveStateOnExit) {
            restoreState(Properties::Instance()->mainWindowState);
        }
    }

    consoleTabulator->setAutoFillBackground(true);
    connect(consoleTabulator, &TabWidget::closeLastTabNotification, this, &MainWindow::close);
    consoleTabulator->setTabPosition((QTabWidget::TabPosition)Properties::Instance()->tabsPos);
    //consoleTabulator->setShellProgram(command);

    const auto menuBarActions = m_menuBar->actions();
    for (const auto& action : menuBarActions)
        menubarOrigTexts << action->text();

    // apply props
    propertiesChanged();

    setupCustomDirs();

    connect(consoleTabulator, &TabWidget::currentTitleChanged, this, &MainWindow::onCurrentTitleChanged);
    connect(menu_Actions, &QMenu::aboutToShow, this, &MainWindow::updateDisabledActions);

    /* The tab should be added after all changes are made to
       the main window; otherwise, the initial prompt might
       get jumbled because of changes in internal geometry. */
    addNewTab(m_config);
}

void MainWindow::rebuildActions()
{
    // Delete all setting-related QObjects
    delete settingOwner;
    settingOwner = new QObject(this);

    // Then create them again
    setup_FileMenu_Actions();
    setup_ActionsMenu_Actions();
    setup_ViewMenu_Actions();
}

MainWindow::~MainWindow()
{
    QTerminalApp::Instance()->removeWindow(this);
}

void MainWindow::enableDropMode()
{
    if (QGuiApplication::platformName() == QStringLiteral("wayland"))
    {
        winId();
        if (QWindow *win = windowHandle())
        {
            m_layerWindow = LayerShellQt::Window::get(win);
            if (m_layerWindow)
            {
                m_layerWindow->setLayer(LayerShellQt::Window::Layer::LayerOverlay);
                m_layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
                LayerShellQt::Window::Anchors anchors = {LayerShellQt::Window::AnchorTop
                                                         | LayerShellQt::Window::AnchorBottom
                                                         | LayerShellQt::Window::AnchorLeft
                                                         | LayerShellQt::Window::AnchorRight};
                m_layerWindow->setAnchors(anchors);
                m_layerWindow->setScope(QStringLiteral("dropdown_terminal"));
                m_layerWindow->setScreenConfiguration(LayerShellQt::Window::ScreenConfiguration::ScreenFromCompositor);
            }
        }
    }

    setWindowFlags(Qt::Dialog | Qt::WindowStaysOnTopHint | Qt::CustomizeWindowHint);

    m_dropLockButton = new QToolButton(this);
    m_dropLockButton->setToolTip(tr("Keep window open when it loses focus"));
    consoleTabulator->setCornerWidget(m_dropLockButton, Qt::BottomRightCorner);
    m_dropLockButton->setCheckable(true);
    m_dropLockButton->connect(m_dropLockButton, &QToolButton::clicked, this, &MainWindow::setKeepOpen);
    setKeepOpen(Properties::Instance()->dropKeepOpen);
    m_dropLockButton->setAutoRaise(true);

    setDropShortcut(Properties::Instance()->dropShortCut);
    realign();
}

void MainWindow::setDropShortcut(const QKeySequence& dropShortCut)
{
    if (!m_dropMode)
        return;

    if (m_dropShortcut.shortcut() != dropShortCut)
    {
        m_dropShortcut.setShortcut(dropShortCut);
        qWarning().noquote() << tr("Press \"%1\" to see the terminal.").arg(dropShortCut.toString());
    }
}

void MainWindow::setup_Action(const char *name, QAction *action, const char *defaultShortcut, const QObject *receiver,
                              const char *slot, QMenu *menu, const QVariant &data)
{
    QList<QKeySequence> shortcuts;

    actions[QLatin1String(name)] = action;
    const auto sequences = Properties::Instance()->getShortcut(QLatin1String(name), QLatin1String(defaultShortcut)).split(QLatin1Char('|'));
    for (const QString &sequenceString : sequences)
        shortcuts.append(QKeySequence::fromString(sequenceString));
    actions[QLatin1String(name)]->setShortcuts(shortcuts);
    actions[QLatin1String(name)]->setShortcutContext(Qt::WidgetWithChildrenShortcut);

    if (receiver)
    {
        connect(actions[QLatin1String(name)], SIGNAL(triggered(bool)), receiver, slot);
        addAction(actions[QLatin1String(name)]);
    }

    if (menu)
        menu->addAction(actions[QLatin1String(name)]);

    if (!data.isNull())
        actions[QLatin1String(name)]->setData(data);
}

void MainWindow::setup_ActionsMenu_Actions()
{
    QVariant data;

    const checkfn checkTabs = &MainWindow::hasMultipleTabs;
    const checkfn checkSubterminals = &MainWindow::hasMultipleSubterminals;
    const checkfn checkHasIndexedTab = &MainWindow::hasIndexedTab;

    menu_Actions->clear();

    setup_Action(CLEAR_TERMINAL, new QAction(QIcon::fromTheme(QStringLiteral("edit-clear")), tr("&Clear Active Terminal"), settingOwner),
                 CLEAR_TERMINAL_SHORTCUT, consoleTabulator, SLOT(clearActiveTerminal()), menu_Actions);

    menu_Actions->addSeparator();

    data.setValue(checkTabs);

    setup_Action(TAB_NEXT, new QAction(QIcon::fromTheme(QStringLiteral("go-next")), tr("&Next Tab"), settingOwner),
                 TAB_NEXT_SHORTCUT, consoleTabulator, SLOT(switchToRight()), menu_Actions, data);

    setup_Action(TAB_PREV, new QAction(QIcon::fromTheme(QStringLiteral("go-previous")), tr("&Previous Tab"), settingOwner),
                 TAB_PREV_SHORTCUT, consoleTabulator, SLOT(switchToLeft()), menu_Actions, data);

    setup_Action(TAB_PREV_HISTORY, new QAction(tr("&Previous Tab in History"), settingOwner),
                 TAB_PREV_HISTORY_SHORTCUT, consoleTabulator, SLOT(switchToPrev()), menu_Actions, data);

    setup_Action(TAB_NEXT_HISTORY, new QAction(tr("&Next Tab in History"), settingOwner),
                 TAB_NEXT_HISTORY_SHORTCUT, consoleTabulator, SLOT(switchToNext()), menu_Actions, data);

    setup_Action(MOVE_LEFT, new QAction(tr("Move Tab &Left"), settingOwner),
                 MOVE_LEFT_SHORTCUT, consoleTabulator, SLOT(moveLeft()), menu_Actions, data);

    setup_Action(MOVE_RIGHT, new QAction(tr("Move Tab &Right"), settingOwner),
                 MOVE_RIGHT_SHORTCUT, consoleTabulator, SLOT(moveRight()), menu_Actions, data);

    data.setValue(checkHasIndexedTab);

    const QString textBase = tr("Tab");
    QMenu *menu_GoTo = new QMenu(tr("Go to"), menu_Actions);
    for (int i=1; i<=10; ++i) {
        QString num = QString::number(i);
        QAction *action = new QAction(textBase + QLatin1Char(' ') + num, settingOwner);
        action->setProperty("tab", i);
        char name[16];
        snprintf(name, sizeof(name), "Tab %d", i);
        setup_Action(name, action, NULL, consoleTabulator, SLOT(onAction()), menu_GoTo, data);
    }
    menu_Actions->addMenu(menu_GoTo);

    menu_Actions->addSeparator();

    setup_Action(SPLIT_HORIZONTAL, new QAction(tr("Split &View Top-Bottom"), settingOwner),
                 nullptr, consoleTabulator, SLOT(splitHorizontally()), menu_Actions);

    setup_Action(SPLIT_VERTICAL, new QAction(tr("Split Vie&w Left-Right"), settingOwner),
                 nullptr, consoleTabulator, SLOT(splitVertically()), menu_Actions);

    data.setValue(checkSubterminals);

    setup_Action(SUB_COLLAPSE, new QAction(tr("&Close Subterminal"), settingOwner),
                 nullptr, consoleTabulator, SLOT(splitCollapse()), menu_Actions, data);

    setup_Action(SUB_TOP, new QAction(QIcon::fromTheme(QStringLiteral("go-up")), tr("&Top Subterminal"), settingOwner),
                 SUB_TOP_SHORTCUT, consoleTabulator, SLOT(switchTopSubterminal()), menu_Actions, data);

    setup_Action(SUB_BOTTOM, new QAction(QIcon::fromTheme(QStringLiteral("go-down")), tr("&Bottom Subterminal"), settingOwner),
                 SUB_BOTTOM_SHORTCUT, consoleTabulator, SLOT(switchBottomSubterminal()), menu_Actions, data);

    setup_Action(SUB_LEFT, new QAction(QIcon::fromTheme(QStringLiteral("go-previous")), tr("L&eft Subterminal"), settingOwner),
                 SUB_LEFT_SHORTCUT, consoleTabulator, SLOT(switchLeftSubterminal()), menu_Actions, data);

    setup_Action(SUB_RIGHT, new QAction(QIcon::fromTheme(QStringLiteral("go-next")), tr("R&ight Subterminal"), settingOwner),
                 SUB_RIGHT_SHORTCUT, consoleTabulator, SLOT(switchRightSubterminal()), menu_Actions, data);


    menu_Actions->addSeparator();

    // Copy and Paste are only added to the table for the sake of bindings at the moment; there is no Edit menu, only a context menu.
    setup_Action(COPY_SELECTION, new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy &Selection"), settingOwner),
                 COPY_SELECTION_SHORTCUT, consoleTabulator, SLOT(copySelection()), menu_Edit);

    setup_Action(PASTE_CLIPBOARD, new QAction(QIcon::fromTheme(QStringLiteral("edit-paste")), tr("Paste Clip&board"), settingOwner),
                 PASTE_CLIPBOARD_SHORTCUT, consoleTabulator, SLOT(pasteClipboard()), menu_Edit);

    setup_Action(PASTE_SELECTION, new QAction(QIcon::fromTheme(QStringLiteral("edit-paste")), tr("Paste S&election"), settingOwner),
                 PASTE_SELECTION_SHORTCUT, consoleTabulator, SLOT(pasteSelection()), menu_Edit);

    setup_Action(ZOOM_IN, new QAction(QIcon::fromTheme(QStringLiteral("zoom-in")), tr("Zoom &in"), settingOwner),
                 ZOOM_IN_SHORTCUT, consoleTabulator, SLOT(zoomIn()), menu_Edit);

    setup_Action(ZOOM_OUT, new QAction(QIcon::fromTheme(QStringLiteral("zoom-out")), tr("Zoom &out"), settingOwner),
                 ZOOM_OUT_SHORTCUT, consoleTabulator, SLOT(zoomOut()), menu_Edit);

    setup_Action(ZOOM_RESET, new QAction(QIcon::fromTheme(QStringLiteral("zoom-original")), tr("Zoom rese&t"), settingOwner),
                 ZOOM_RESET_SHORTCUT, consoleTabulator, SLOT(zoomReset()), menu_Edit);

    menu_Actions->addSeparator();

    setup_Action(FIND, new QAction(QIcon::fromTheme(QStringLiteral("edit-find")), tr("&Find..."), settingOwner),
                 FIND_SHORTCUT, this, SLOT(find()), menu_Actions);

    setup_Action(HANDLE_HISTORY, new QAction(QIcon::fromTheme(QStringLiteral("handle-history")), tr("Handle history..."), settingOwner),
                 NULL, this, SLOT(handleHistory()), menu_Actions);

#if 0
    act = new QAction(this);
    act->setSeparator(true);

    // TODO/FIXME: unimplemented for now
    act = new QAction(tr("&Save Session"), this);
    // do not use sequences for this task - it collides with eg. mc shortcuts
    // and mainly - it's not used too often
    //act->setShortcut(QKeySequence::Save);
    connect(act, SIGNAL(triggered()), consoleTabulator, SLOT(saveSession()));

    act = new QAction(tr("&Load Session"), this);
    // do not use sequences for this task - it collides with eg. mc shortcuts
    // and mainly - it's not used too often
    //act->setShortcut(QKeySequence::Open);
    connect(act, SIGNAL(triggered()), consoleTabulator, SLOT(loadSession()));
#endif

    setup_Action(TOGGLE_MENU, new QAction(tr("&Toggle Menu"), settingOwner),
                 TOGGLE_MENU_SHORTCUT, this, SLOT(toggleMenu()));
    // this is correct - add action to main window - not to menu to keep toggle working

    // Add global rename current session shortcut
    setup_Action(RENAME_SESSION, new QAction(tr("Rename session"), settingOwner),
                 RENAME_SESSION_SHORTCUT, consoleTabulator, SLOT(renameCurrentSession()));
    // this is correct - add action to main window - not to menu

}

void MainWindow::setup_FileMenu_Actions()
{
    menu_File->clear();
    setup_Action(ADD_TAB, new QAction(QIcon::fromTheme(QStringLiteral("list-add")), tr("&New Tab"), settingOwner),
                 ADD_TAB_SHORTCUT, this, SLOT(addNewTab()), menu_File);

    if (presetsMenu == nullptr) {
        presetsMenu = new QMenu(tr("New Tab From &Preset"), this);
        auto a = presetsMenu->addAction(QIcon(), tr("1 &Terminal"));
        connect(a, &QAction::triggered, consoleTabulator, [this]() {
            consoleTabulator->addNewTab(m_config);
        });
        presetsMenu->addAction(QIcon(), tr("2 &Horizontal Terminals"),
                               consoleTabulator, SLOT(preset2Horizontal()));
        presetsMenu->addAction(QIcon(), tr("2 &Vertical Terminals"),
                               consoleTabulator, SLOT(preset2Vertical()));
        presetsMenu->addAction(QIcon(), tr("4 Terminal&s"),
                               consoleTabulator, SLOT(preset4Terminals()));
    }

    menu_File->addMenu(presetsMenu);

    setup_Action(CLOSE_TAB, new QAction(QIcon::fromTheme(QStringLiteral("list-remove")), tr("&Close Tab"), settingOwner),
                 CLOSE_TAB_SHORTCUT, consoleTabulator, SLOT(removeCurrentTab()), menu_File);

    setup_Action(NEW_WINDOW, new QAction(QIcon::fromTheme(QStringLiteral("window-new")), tr("&New Window"), settingOwner),
                 NEW_WINDOW_SHORTCUT, this, SLOT(newTerminalWindow()), menu_File);

    menu_File->addSeparator();

    setup_Action(PREFERENCES, new QAction(tr("&Preferences..."), settingOwner), "", this, SLOT(actProperties_triggered()), menu_File);

    menu_File->addSeparator();

    setup_Action(QUIT, new QAction(QIcon::fromTheme(QStringLiteral("application-exit")), tr("&Quit"), settingOwner), "", this, SLOT(close()), menu_File);
}

void MainWindow::setup_ViewMenu_Actions()
{
    menu_Window->clear();
    QAction *hideBordersAction = new QAction(tr("&Hide Window Borders"), settingOwner);
    hideBordersAction->setCheckable(true);
    hideBordersAction->setVisible(!m_dropMode);

    hideBordersAction->setChecked(Properties::Instance()->borderless);
    if (!m_dropMode) // dropdown mode doesn't need any change
    {
        if (!testAttribute(Qt::WA_WState_Created)) // called by c-tor
        {
            if (Properties::Instance()->borderless)
                setWindowFlags(windowFlags() ^ Qt::FramelessWindowHint);
        }
        else if (Properties::Instance()->borderless != windowFlags().testFlag(Qt::FramelessWindowHint))
            QTimer::singleShot(0, this, &MainWindow::toggleBorderless); // called by PropertiesDialog
    }
    setup_Action(HIDE_WINDOW_BORDERS, hideBordersAction,
                 nullptr, this, SLOT(toggleBorderless()), menu_Window);

    QAction *showTabBarAction = new QAction(tr("&Show Tab Bar"), settingOwner);
    //toggleTabbar->setObjectName("toggle_TabBar");
    showTabBarAction->setCheckable(true);
    showTabBarAction->setChecked(!Properties::Instance()->tabBarless);
    setup_Action(SHOW_TAB_BAR, showTabBarAction,
                 nullptr, this, SLOT(toggleTabBar()), menu_Window);
    toggleTabBar();

    QAction *toggleFullscreen = new QAction(tr("Fullscreen"), settingOwner);
    toggleFullscreen->setCheckable(true);
    toggleFullscreen->setChecked(false);
    setup_Action(FULLSCREEN, toggleFullscreen,
                 FULLSCREEN_SHORTCUT, this, SLOT(showFullscreen(bool)), menu_Window);

    setup_Action(TOGGLE_BOOKMARKS, new QAction(tr("Toggle Bookmarks"), settingOwner),
                 TOGGLE_BOOKMARKS_SHORTCUT, this, SLOT(toggleBookmarks()), menu_Window);

    menu_Window->addSeparator();

    /* tabs position */
    if (tabPosition == nullptr) {
        tabPosition = new QActionGroup(this);
        QAction *tabBottom = new QAction(tr("&Bottom"), this);
        QAction *tabTop = new QAction(tr("&Top"), this);
        QAction *tabRight = new QAction(tr("&Right"), this);
        QAction *tabLeft = new QAction(tr("&Left"), this);
        tabPosition->addAction(tabTop);
        tabPosition->addAction(tabBottom);
        tabPosition->addAction(tabLeft);
        tabPosition->addAction(tabRight);

        for(int i = 0; i < tabPosition->actions().size(); ++i)
            tabPosition->actions().at(i)->setCheckable(true);
    }


    if( tabPosition->actions().count() > Properties::Instance()->tabsPos )
        tabPosition->actions().at(Properties::Instance()->tabsPos)->setChecked(true);

    connect(tabPosition, &QActionGroup::triggered,
            consoleTabulator, &TabWidget::changeTabPosition);

    if (tabPosMenu == nullptr) {
        tabPosMenu = new QMenu(tr("&Tabs Layout"), menu_Window);
        tabPosMenu->setObjectName(QStringLiteral("tabPosMenu"));

        for(int i=0; i < tabPosition->actions().size(); ++i) {
            tabPosMenu->addAction(tabPosition->actions().at(i));
        }

        connect(menu_Window, &QMenu::hovered,
                this, &MainWindow::updateActionGroup);
    }
    menu_Window->addMenu(tabPosMenu);
    /* */

    /* Scrollbar */
    if (scrollBarPosition == nullptr) {
        scrollBarPosition = new QActionGroup(this);
        QAction *scrollNone = new QAction(tr("&None"), this);
        QAction *scrollRight = new QAction(tr("&Right"), this);
        QAction *scrollLeft = new QAction(tr("&Left"), this);
        /* order of insertion is dep. on QTermWidget::ScrollBarPosition enum */
        scrollBarPosition->addAction(scrollNone);
        scrollBarPosition->addAction(scrollLeft);
        scrollBarPosition->addAction(scrollRight);

        for(int i = 0; i < scrollBarPosition->actions().size(); ++i)
            scrollBarPosition->actions().at(i)->setCheckable(true);

        if( Properties::Instance()->scrollBarPos < scrollBarPosition->actions().size() )
            scrollBarPosition->actions().at(Properties::Instance()->scrollBarPos)->setChecked(true);
        connect(scrollBarPosition, &QActionGroup::triggered,
                consoleTabulator, &TabWidget::changeScrollPosition);

    }
    if (scrollPosMenu == nullptr) {
        scrollPosMenu = new QMenu(tr("S&crollbar Layout"), menu_Window);
        scrollPosMenu->setObjectName(QStringLiteral("scrollPosMenu"));

        for(int i=0; i < scrollBarPosition->actions().size(); ++i) {
            scrollPosMenu->addAction(scrollBarPosition->actions().at(i));
        }
    }

    menu_Window->addMenu(scrollPosMenu);

    /* Keyboard cursor shape */
    if (keyboardCursorShape == nullptr) {
        keyboardCursorShape = new QActionGroup(this);
        QAction *block = new QAction(tr("&BlockCursor"), this);
        QAction *underline = new QAction(tr("&UnderlineCursor"), this);
        QAction *ibeam = new QAction(tr("&IBeamCursor"), this);

        /* order of insertion is dep. on QTermWidget::KeyboardCursorShape enum */
        keyboardCursorShape->addAction(block);
        keyboardCursorShape->addAction(underline);
        keyboardCursorShape->addAction(ibeam);
        for(int i = 0; i < keyboardCursorShape->actions().size(); ++i)
            keyboardCursorShape->actions().at(i)->setCheckable(true);

        if( Properties::Instance()->keyboardCursorShape < keyboardCursorShape->actions().size() )
            keyboardCursorShape->actions().at(Properties::Instance()->keyboardCursorShape)->setChecked(true);

        connect(keyboardCursorShape, &QActionGroup::triggered,
                consoleTabulator, &TabWidget::changeKeyboardCursorShape);
    }

    if (keyboardCursorShapeMenu == nullptr) {
        keyboardCursorShapeMenu = new QMenu(tr("&Keyboard Cursor Shape"), menu_Window);
        keyboardCursorShapeMenu->setObjectName(QStringLiteral("keyboardCursorShapeMenu"));

        for(int i=0; i < keyboardCursorShape->actions().size(); ++i) {
            keyboardCursorShapeMenu->addAction(keyboardCursorShape->actions().at(i));
        }
    }

    menu_Window->addMenu(keyboardCursorShapeMenu);
}

void MainWindow::setupCustomDirs()
{
    const QString appName = QCoreApplication::applicationName();
    QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, appName,
                                                       QStandardPaths::LocateDirectory);

    dirs.removeDuplicates(); // QStandardPaths::locateAll() produces duplicates

    for (const QString& dir : std::as_const(dirs)) {
        TermWidgetImpl::addCustomColorSchemeDir(dir + QLatin1String("/color-schemes"));
    }
    // FIXME: To be deprecated and then removed
    const QSettings settings;
    const QString dir = QFileInfo(settings.fileName()).canonicalPath() + QLatin1String("/color-schemes");
    TermWidgetImpl::addCustomColorSchemeDir(dir);
}

void MainWindow::on_consoleTabulator_currentChanged(int)
{
}

void MainWindow::toggleTabBar()
{
    Properties::Instance()->tabBarless = !actions[QLatin1String(SHOW_TAB_BAR)]->isChecked();
    consoleTabulator->showHideTabBar();
}

void MainWindow::toggleBorderless()
{
    setWindowFlags(windowFlags() ^ Qt::FramelessWindowHint);
    show();
    setWindowState(Qt::WindowActive); /* don't loose focus on the window */
    Properties::Instance()->borderless = actions[QLatin1String(HIDE_WINDOW_BORDERS)]->isChecked();
    realign();
}

void MainWindow::toggleMenu()
{
    m_menuBar->setVisible(!m_menuBar->isVisible());
    Properties::Instance()->menuVisible = m_menuBar->isVisible();
}

void MainWindow::showFullscreen(bool fullscreen)
{
    if(fullscreen)
        setWindowState(windowState() | Qt::WindowFullScreen);
    else
        setWindowState(windowState() & ~Qt::WindowFullScreen);
}

void MainWindow::toggleBookmarks()
{
    m_bookmarksDock->toggleViewAction()->trigger();
    if (m_bookmarksDock->isVisible())
    { // give the focus to the bookmarks dock
        if (m_bookmarksDock->isFloating())
        {
            m_bookmarksDock->activateWindow();
        }
        m_bookmarksDock->widget()->setFocus();
    }
}

bool MainWindow::closePrompt(const QString &title, const QString &text)
{
    QDialog * dia = new QDialog(this);
    dia->setObjectName(QStringLiteral("exitDialog"));
    dia->setWindowTitle(title);

    QCheckBox * dontAskCheck = new QCheckBox(tr("Do not ask again"), dia);
    QDialogButtonBox * buttonBox = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, Qt::Horizontal, dia);
    buttonBox->button(QDialogButtonBox::Yes)->setDefault(true);

    connect(buttonBox, &QDialogButtonBox::accepted, dia, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, dia, &QDialog::reject);

    QVBoxLayout * lay = new QVBoxLayout();
    lay->addWidget(new QLabel(QStringLiteral("<center>") + text + QStringLiteral("</center>")));
    lay->addWidget(new QLabel(QStringLiteral("<center><i>") + tr("A process is running.") + QStringLiteral("</i></center>")));
    lay->addStretch();
    lay->addWidget(dontAskCheck);
    lay->addWidget(buttonBox);
    dia->setLayout(lay);

    bool res(dia->exec() == QDialog::Accepted);
    if (res)
    {
        Properties::Instance()->askOnExit = !dontAskCheck->isChecked();
    }
    dia->deleteLater();
    return res;
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
    if (!Properties::Instance()->askOnExit
        || consoleTabulator->count() == 0
        // the session is ended explicitly (e.g., by ctrl-d); prompt doesn't make sense
        || consoleTabulator->terminalHolder()->findChildren<TermWidget*>().count() == 0
        // there is no running process
        || !consoleTabulator->hasRunningProcess()
        // ask user for canceling otherwise
        || closePrompt(tr("Exit QTerminal"), tr("Are you sure you want to exit?")))
    {
        disconnect(m_bookmarksDock, &QDockWidget::visibilityChanged,
                   this, &MainWindow::bookmarksDock_visibilityChanged); // prevent crash
        // do not save state and geometry in drop mode
        if (!m_dropMode)
        {
            if (Properties::Instance()->savePosOnExit)
            {
                Properties::Instance()->mainWindowPosition = pos();
            }
            if (Properties::Instance()->saveSizeOnExit)
            {
                Properties::Instance()->mainWindowSize = size();
                Properties::Instance()->windowMaximized = isMaximized();
            }
            if (Properties::Instance()->saveStateOnExit)
            {
                Properties::Instance()->mainWindowState = saveState();
            }
        }
        rebuildActions(); // shortcuts may have changed by another running instance
        Properties::Instance()->saveSettings();
        for (int i = consoleTabulator->count(); i > 0; --i)
        {
            consoleTabulator->removeTab(i - 1);
        }
        ev->accept();
    }
    else
    {
        ev->ignore();
    }
}

void MainWindow::actAbout_triggered()
{
     QMessageBox::about(this, tr("About"),
                     QStringLiteral("<center><b><big>QTerminal %1</big></b></center><br>").arg(qApp->applicationVersion())
                     + tr("A lightweight and powerful multiplatform terminal emulator")
                     + QStringLiteral("<br><br>")
                     + tr("Copyright (C) ") + tr("2013-2025")
                     + QStringLiteral("<br><a href='https://lxqt-project.org'>")
                     + tr("LXQt Project")
                     + QStringLiteral("</a><br><br>")
                     + tr("Development: ")
                     + QStringLiteral("<a href='https://github.com/lxqt/qterminal'>https://github.com/lxqt/qterminal</a><br><br>"));
}

void MainWindow::actProperties_triggered()
{
    PropertiesDialog p(this);
    connect(&p, &PropertiesDialog::propertiesChanged, this, &MainWindow::propertiesChanged);
    p.exec();
}

void MainWindow::propertiesChanged()
{
    rebuildActions();

    QApplication::setStyle(Properties::Instance()->guiStyle);
    consoleTabulator->setTabPosition((QTabWidget::TabPosition)Properties::Instance()->tabsPos);
    consoleTabulator->propertiesChanged();
    setDropShortcut(Properties::Instance()->dropShortCut);


    const auto menuBarActions = m_menuBar->actions();
    if (Properties::Instance()->noMenubarAccel)
    {
        for (auto& action : menuBarActions)
        {
            QString txt = action->text();
            Properties::removeAccelerator(txt);
            action->setText(txt);
        }
    }
    else if (menubarOrigTexts.size() == menuBarActions.size())
    {
        int i = 0;
        for (auto& action : menuBarActions)
        {
            action->setText(menubarOrigTexts.at(i));
            ++i;
        }
    }

    m_menuBar->setVisible(Properties::Instance()->menuVisible);

    m_bookmarksDock->setVisible(Properties::Instance()->useBookmarks
                                && Properties::Instance()->bookmarksVisible);
    actions[QLatin1String(TOGGLE_BOOKMARKS)]->setVisible(Properties::Instance()->useBookmarks);

    if (Properties::Instance()->useBookmarks)
    {
        qobject_cast<BookmarksWidget*>(m_bookmarksDock->widget())->setup();
    }

    onCurrentTitleChanged(consoleTabulator->currentIndex());

    setKeepOpen(Properties::Instance()->dropKeepOpen);

    realign();
}

void MainWindow::realign()
{
    if (m_dropMode)
    {
        if (m_layerWindow)
        {
            return; // done in showEvent
        }
        QScreen *appScreen = QGuiApplication::screenAt(QCursor::pos());
        if(appScreen == nullptr)
        {
            appScreen = QGuiApplication::primaryScreen();
        }
        const QRect desktop = appScreen->availableGeometry();
        QRect g = QRect(desktop.x(),
                        desktop.y(),
                        desktop.width()  * Properties::Instance()->dropWidth  / 100,
                        desktop.height() * Properties::Instance()->dropHeight / 100);
        g.moveCenter(desktop.center());
        // do not use 0 here - we need to calculate with potential panel on top
        g.moveTop(desktop.top());
        if (g != geometry())
        {
            setGeometry(g);
        }
    }
}

void MainWindow::updateActionGroup(QAction *a)
{
    if (a->parent()->objectName() == tabPosMenu->objectName()) {
        tabPosition->actions().at(Properties::Instance()->tabsPos)->setChecked(true);
    }
}

void MainWindow::showHide()
{
    // don't toggle the drop-down terminal when it has a modal dialog
    const auto dialogs = findChildren<QDialog*>();
    for (const auto& dialog : dialogs)
    {
        if(dialog->isModal())
        {
            return;
        }
    }

    if (isVisible())
    {
        hide();
    }
    else
    {
        // The checked state of the fullscreen action should be reset; otherwise, its shortcut
        // might need to be pressed twice later to make the window fullscreen. We don't consult
        // "isFullScreen()" because it will return "false" if the window has been deactivated.
        if (auto a = actions.value(QLatin1String(FULLSCREEN)))
        {
            a->setChecked(false);
        }
        realign();
        show();
        activateWindow();
    }
}

void MainWindow::setKeepOpen(bool value)
{
    Properties::Instance()->dropKeepOpen = value;
    if (!m_dropLockButton)
        return;

    if (value)
        m_dropLockButton->setIcon(QIcon::fromTheme(QStringLiteral("object-locked")));
    else
        m_dropLockButton->setIcon(QIcon::fromTheme(QStringLiteral("object-unlocked")));

    m_dropLockButton->setChecked(value);
}

void MainWindow::find()
{
    // A bit ugly perhaps with 4 levels of indirection...
    consoleTabulator->terminalHolder()->currentTerminal()->impl()->toggleShowSearchBar();
}

void MainWindow::handleHistory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString fn = dir + QLatin1String("/qterminal.history.") + QString::number(QCoreApplication::applicationPid());
    QFile file(fn);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open" << file.fileName() << "for writing";
        return;
    }
    TermWidgetImpl *impl = consoleTabulator->terminalHolder()->currentTerminal()->impl();
    impl->saveHistory(&file);
    file.close();
    QStringList args = Properties::Instance()->handleHistoryCommand.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (args.isEmpty())
        return;

    QString command = args[0];
    args.removeFirst();
    args << fn;
    if (!QProcess::startDetached(command, args)) {
        qDebug() << "Failed to start command" << command << args;
    }

}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate)
    {
        if (m_dropMode &&
            !Properties::Instance()->dropKeepOpen &&
            qApp->activeWindow() == nullptr)
        {
            // On Wayland and with a modal dialog, the dropdown window can be activated by
            // clicking inside it and then deactivated by clicking on another window (see below).
            if (!m_layerWindow || m_layerWindow->layer() == LayerShellQt::Window::Layer::LayerOverlay)
            {
                hide();
            }
        }
    }
    // A workaround for the modal dialogs of the dropdown window on Wayland.
    else if (event->type() == QEvent::WindowBlocked && m_layerWindow && m_dropMode)
    {
        if (auto dialog = qobject_cast<QDialog*>(qApp->activeModalWidget()))
        {
            dialog->winId();
            if (QWindow *win = dialog->windowHandle())
            {
                if (LayerShellQt::Window *layershell = LayerShellQt::Window::get(win))
                {
                    layershell->setLayer(LayerShellQt::Window::Layer::LayerOverlay);
                    layershell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
                    LayerShellQt::Window::Anchors anchors = {LayerShellQt::Window::AnchorTop};
                    layershell->setAnchors(anchors);
                    layershell->setScreenConfiguration(LayerShellQt::Window::ScreenConfiguration::ScreenFromCompositor);
                    layershell->setScope(QStringLiteral("dialog"));
                    // For preventing the dialog from going behind the dropdown window if
                    // the latter is clicked, the window is set on the top layer temporarily.
                    // It will be set on the overlay layer again when the dialog exits.
                    m_layerWindow->setLayer(LayerShellQt::Window::Layer::LayerTop);
                }
            }
        }
    }
    else if (event->type() == QEvent::WindowUnblocked && m_layerWindow && m_dropMode)
    {
        // Restore the overlay layer.
        m_layerWindow->setLayer(LayerShellQt::Window::Layer::LayerOverlay);
    }

    return QMainWindow::event(event);
}

void MainWindow::showEvent(QShowEvent* event)
{
    if (m_layerWindow && m_dropMode)
    {
        const QRect desktop = windowHandle()->screen()->availableGeometry();
        int hMargin = desktop.width() * (100 - Properties::Instance()->dropWidth) / 200;
        int vMargin = desktop.height() * (100 - Properties::Instance()->dropHeight) / 100;
        m_layerWindow->setMargins(QMargins(hMargin, 0, hMargin, vMargin));
    }
    QMainWindow::showEvent(event);
}

void MainWindow::newTerminalWindow()
{
    TerminalConfig cfg;
    TermWidgetHolder *ch = consoleTabulator->terminalHolder();
    if (ch)
        cfg.provideCurrentDirectory(ch->currentTerminal()->impl()->workingDirectory());

    if (m_dropMode)
    { // the dropdown process has only one (dropdown) main window
        QStringList args;
        args <<  QStringLiteral("-w") << cfg.getWorkingDirectory();
        QString profile = Properties::Instance()->profile();
        if (!profile.isEmpty())
        {
            args << QStringLiteral("-p") << profile;
        }
        QProcess::startDetached(QStringLiteral("qterminal"), args);
    }
    else
    {
        MainWindow *w = new MainWindow(cfg, false);
        w->show();
    }
}

void MainWindow::bookmarksWidget_callCommand(const QString& cmd)
{
    if (m_bookmarksDock->isFloating())
    {
        activateWindow();
    }
    consoleTabulator->terminalHolder()->currentTerminal()->impl()->sendText(cmd);
    // the focus proxy (TermWidgetImpl) should be checked because it's nullptr with "exit"
    if (consoleTabulator->terminalHolder()->currentTerminal()->focusProxy() != nullptr) {
        consoleTabulator->terminalHolder()->currentTerminal()->setFocus();
    }
}

void MainWindow::bookmarksDock_visibilityChanged(bool visible)
{
    Properties::Instance()->bookmarksVisible = visible;
    if (!visible && consoleTabulator->terminalHolder()
        && consoleTabulator->terminalHolder()->currentTerminal()->focusProxy() != nullptr)
    { // this is especially needed in the drop-down mode
        consoleTabulator->terminalHolder()->currentTerminal()->setFocus();
    }
}

void MainWindow::addNewTab(TerminalConfig cfg)
{
    if (cfg.hasCommand())
    {
        // do not create subterminals if there is a command (-e option)
        consoleTabulator->addNewTab(cfg);
        return;
    }

    if (Properties::Instance()->terminalsPreset == 3)
        consoleTabulator->preset4Terminals();
    else if (Properties::Instance()->terminalsPreset == 2)
        consoleTabulator->preset2Vertical();
    else if (Properties::Instance()->terminalsPreset == 1)
        consoleTabulator->preset2Horizontal();
    else
        consoleTabulator->addNewTab(cfg);
    // disabled actions are updated by TabWidget::onCurrentChanged()
}

void MainWindow::onCurrentTitleChanged(int index)
{
    QString title;
    QIcon icon;
    if (-1 != index)
    {
        title = consoleTabulator->tabText(index);
        icon = consoleTabulator->tabIcon(index);
    }
    setWindowTitle(title.isEmpty() || !Properties::Instance()->changeWindowTitle ? QStringLiteral("QTerminal") : title);
    setWindowIcon(icon.isNull() || !Properties::Instance()->changeWindowIcon ? QIcon::fromTheme(QStringLiteral("utilities-terminal")) : icon);
}

bool MainWindow::hasMultipleTabs(QAction *)
{
    return consoleTabulator->findChildren<TermWidgetHolder*>().count() > 1;
}

bool MainWindow::hasMultipleSubterminals(QAction *)
{
    return consoleTabulator->terminalHolder()->findChildren<TermWidget*>().count() > 1;
}

bool MainWindow::hasIndexedTab(QAction *action)
{
    bool ok = false;
    const int index = action->property("tab").toInt(&ok);
    Q_ASSERT(ok);
    static_cast<void>(ok);
    return consoleTabulator->findChildren<TermWidgetHolder*>().count() >= index;
}

void MainWindow::updateDisabledActions()
{
    std::function<void(const QList<QAction *> &)> enableActions = [this, &enableActions](const QList<QAction *> &actions) {
        for (QAction *action : actions) {
            if (!action->data().isNull()) {
                const checkfn check = action->data().value<checkfn>();
                action->setEnabled(check(*this, action));
            } else if (QMenu *menu = action->menu()) {
                enableActions(menu->actions());
            }
        }
    };

    enableActions(menu_Actions->actions());
}

QMap< QString, QAction * >& MainWindow::leaseActions() {
    return actions;
}
#ifdef HAVE_QDBUS

QDBusObjectPath MainWindow::getActiveTab()
{
    return qobject_cast<TermWidgetHolder*>(consoleTabulator->currentWidget())->getDbusPath();
}

QList<QDBusObjectPath> MainWindow::getTabs()
{
    QList<QDBusObjectPath> tabs;
    for (int i = 0; i<consoleTabulator->count(); ++i)
    {
        tabs.push_back(qobject_cast<TermWidgetHolder*>(consoleTabulator->widget(i))->getDbusPath());
    }
    return tabs;

}

QDBusObjectPath MainWindow::newTab(const QHash<QString,QVariant> &termArgs)
{
    TerminalConfig cfg = TerminalConfig::fromDbus(termArgs);
    int idx = consoleTabulator->addNewTab(cfg);
    return qobject_cast<TermWidgetHolder*>(consoleTabulator->widget(idx))->getDbusPath();
}

void MainWindow::closeWindow()
{
    close();
}

#endif
