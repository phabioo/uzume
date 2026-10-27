# uzume_ui: QML module Uzume.App (static, linked into uzume_app).
# C++ glue (controller, models, GameView, audio) and QML screens of the UI.
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_library(uzume_ui STATIC)
qt_add_qml_module(uzume_ui
  URI Uzume.App
  VERSION 1.0
  RESOURCE_PREFIX /qt/qml
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Uzume/App
  SOURCES
    uifonts.h uifonts.cpp
    inputmapping.h inputmapping.cpp
    timeformat.h timeformat.cpp
    screenlayout.h screenlayout.cpp
    diagnosticsmodel.h diagnosticsmodel.cpp
    emulationcontroller.h emulationcontroller.cpp
    audiocontroller.h audiocontroller.cpp
    controllerscontroller.h controllerscontroller.cpp
    controllersglyphs.h controllersglyphs.cpp
    audiooutput.h audiooutput.cpp
    gamesession.h gamesession.cpp
    gpuencodebridge.h gpuencodebridge.cpp
    gpudisplaybridge.h gpudisplaybridge.cpp
    gameview.h gameview.cpp
    librarymodel.h librarymodel.cpp
    playercontroller.h playercontroller.cpp
    playphase.h
    corecatalog.h corecatalog.cpp
    coreprober.h coreprober.cpp
    gamedetail.h gamedetail.cpp
    hubpresenter.h hubpresenter.cpp
    gamestarter.h gamestarter.cpp
    uploadcontroller.h uploadcontroller.cpp
    sessioncontroller.h sessioncontroller.cpp
    remoteview.h remoteview.cpp
    updatescontroller.h updatescontroller.cpp
    reportcontroller.h reportcontroller.cpp
    mcpcontroller.h mcpcontroller.cpp
    admincontroller.h admincontroller.cpp
    savehistorycontroller.h savehistorycontroller.cpp
    localhubcontroller.h localhubcontroller.cpp
    pluginruntime.h pluginruntime.cpp
    pluginscontroller.h pluginscontroller.cpp
    pluginemulation.h pluginemulation.cpp
    keyedrowsmodel.h keyedrowsmodel.cpp
    runscontroller.h runscontroller.cpp
    personalcontroller.h personalcontroller.cpp
    screenshotcontroller.h screenshotcontroller.cpp
    localstoragecontroller.h localstoragecontroller.cpp
    variantscontroller.h variantscontroller.cpp
    screenshotimageprovider.h screenshotimageprovider.cpp
    connectioncheckcontroller.h connectioncheckcontroller.cpp
    onboardingcontroller.h onboardingcontroller.cpp
    accountscontroller.h accountscontroller.cpp
    pluginimageprovider.h pluginimageprovider.cpp
  QML_FILES
    Main.qml
    Theme.qml
    UzLabel.qml
    UzMono.qml
    UzButton.qml
    UzToggle.qml
    UzField.qml
    UzTag.qml
    UzChip.qml
    UzSpinner.qml
    ShellColumn.qml
    UzColumnNav.qml
    UzColumnNavRow.qml
    UzCaptureField.qml
    UzPageHints.qml
    UzMark.qml
    UzLockup.qml
    StatusDot.qml
    Eyebrow.qml
    HubCard.qml
    MoreMenu.qml
    StepCard.qml
    ShareDialogs.qml
    ConnectionScreen.qml
    PairingScreen.qml
    StartFrame.qml
    StartPage.qml
    StartHead.qml
    StartField.qml
    StartNotice.qml
    StartChoiceRow.qml
    StartSignInForm.qml
    StartInviteForm.qml
    StartApprovalBlock.qml
    StartWelcomePage.qml
    StartPlayPage.qml
    StartFolderDialog.qml
    StartSetupWherePage.qml
    StartLocalHubPage.qml
    StartAddressPage.qml
    StartCheckingPage.qml
    StartSecurityPage.qml
    StartMessagePage.qml
    StartSignInPage.qml
    StartInvitePage.qml
    StartApprovalPage.qml
    StartLinkReadyPage.qml
    StartLinkChoicePage.qml
    StartSetupAccountPage.qml
    ForgotPasswordDialog.qml
    StartScreen.qml
    PinPad.qml
    SwitchPeoplePage.qml
    SwitchPinPage.qml
    SwitchQuitPage.qml
    SwitchPasswordPage.qml
    SwitchAddPage.qml
    SwitchRemovePage.qml
    SwitchUserScreen.qml
    UzAppNav.qml
    UzNavItem.qml
    UzNavGroupHead.qml
    GameTile.qml
    GameCard.qml
    LibraryScreen.qml
    GameGrid.qml
    GameScreen.qml
    SettingsScreen.qml
    SettingsAccountSection.qml
    ConflictDialog.qml
    StartConfirmDialog.qml
    RollbackDialog.qml
    UzSegmented.qml
    SessionsSection.qml
    GamePage.qml
    GamePanel.qml
    GameHeader.qml
    GameButton.qml
    GameIcon.qml
    SharePill.qml
    AnchoredPopover.qml
    PopoverItem.qml
    SessionPicker.qml
    MultiviewArea.qml
    TileStateCard.qml
    DiagnosticsOverlay.qml
    LayoutSwitch.qml
    BottomScreenWindow.qml
    KeyHintButton.qml
    UzSelect.qml
    UzModelSelect.qml
    UzSettingsRow.qml
    EmulationContent.qml
    ControllersContent.qml
    HotkeysContent.qml
    TableHeadCell.qml
    BindingText.qml
    PadGlyph.qml
    UploadFileDialog.qml
    SaveSummary.qml
    SavesView.qml
    SaveRow.qml
    SortControl.qml
    SaveFileDialog.qml
    CollectLogsDialog.qml
    ReportProblemDialog.qml
    ReportSaveDialog.qml
    LocalHubForm.qml
    LocalHubCard.qml
    LocalFolderDialog.qml
    LibraryFolderDialog.qml
    LibraryFoldersSection.qml
    LocalStorageBlock.qml
    PluginFolderDialog.qml
    PluginFileDialog.qml
    PluginOriginPill.qml
    PluginsSection.qml
    RunsScreen.qml
    RunAvatars.qml
    RunCheckRow.qml
    UnverifiedNote.qml
    GamePickList.qml
    RunJoinDialog.qml
    ConnectionCheckDialog.qml
    RunStartDialog.qml
    PluginConsole.qml
    PluginFocusable.qml
    PluginImage.qml
    PluginConfirmDialog.qml
    PluginConsentDialog.qml
    PluginPickDialog.qml
    PluginButton.qml
    PluginToggle.qml
    PluginSegmented.qml
    PluginSelect.qml
    PluginTextField.qml
    PluginTextArea.qml
    PluginList.qml
    PluginTable.qml
    PluginMapView.qml
    PluginChild.qml
    PluginBlock.qml
    PluginDetailSection.qml
    PluginDetailSheet.qml
    PluginRunSection.qml
    PluginTabIcon.qml
    PluginMenu.qml
    PluginPanelContent.qml
    PluginScreenSheet.qml
    PluginHudWidget.qml
    PluginHudStack.qml
    PluginHudLayer.qml
    PluginToast.qml
    PluginTileStrip.qml
    UzFocusRing.qml
    UzGlyph.qml
    UzIconButton.qml
    UzKeyCap.qml
    UzKbd.qml
    UzGamepadHint.qml
    UzHintRow.qml
    UzDisclosure.qml
    UzOpenColumn.qml
    UzHelpColumn.qml
    UzRegisterMarks.qml
    UzSectionLabel.qml
    UzStorageBar.qml
    UzPageHeader.qml
    UzTabs.qml
    UzVoiceMark.qml
    UzVoiceBars.qml
    UzAvatar.qml
    UzAvatarStack.qml
    UzStatusIcon.qml
    UzStatusLine.qml
    UzEnso.qml
    UzStateBlock.qml
    UzProgress.qml
    UzFilterChip.qml
    UzSystemChip.qml
    UzSlider.qml
    UzCheckbox.qml
    UzDialog.qml
    UzPopover.qml
    UzToast.qml
    UzStageDiagnostics.qml
    UzDiagRows.qml
    UzHotkeySheet.qml
    UzMenu.qml
    UzMenuList.qml
    UzMenuItem.qml
    UzConfirmDialog.qml
    UzTable.qml
    UzTableRow.qml
    UzMetric.qml
    AdminPage.qml
    AdminCall.qml
    AdminPeople.qml
    AdminAskDialog.qml
    CollectionDialogs.qml
    GameSettingsDialog.qml
    GameSettingsContent.qml
    GameQuickMenu.qml
    ScreenshotStrip.qml
    ScreenshotViewer.qml
    ScreenshotSaveDialog.qml
    LibraryAdminBar.qml
    LibraryContinue.qml
    LibraryNotices.qml
    LibraryAdminDetails.qml
    SavesAdminSwitch.qml
    SavesAdminSheet.qml
    SavesAdminDetails.qml
    RunsAdminPanel.qml
    WhatsNewDialog.qml
    SetupAssistant.qml
    ServerProblemLine.qml
    AdminSaveDialog.qml
    ServerScreen.qml
    ServerStatusPage.qml
    ServerUsersPage.qml
    ServerUsersPeople.qml
    ServerUsersInvites.qml
    ServerUsersDevices.qml
    ServerUsersDialogs.qml
    ServerReportsPage.qml
    ServerSystemsPage.qml
    ServerSystemsFileDialog.qml
    ServerPluginsPage.qml
    ServerSettingsPane.qml
    ServerGamesPage.qml
    ServerSettingsGames.qml
    ServerSettingsGeneral.qml
    ServerSettingsPage.qml
    ServerSettingsNetwork.qml
    ServerSettingsSignIn.qml
    ServerSettingsUpdates.qml
    ServerSettingsBackups.qml
    ServerSettingsStorage.qml
    ServerSettingsDiagnostics.qml
  RESOURCES
    fonts/IBMPlexSans-Light.ttf
    fonts/IBMPlexSans-Regular.ttf
    fonts/IBMPlexSans-Medium.ttf
    fonts/IBMPlexSans-SemiBold.ttf
    fonts/IBMPlexMono-Regular.ttf
    fonts/IBMPlexMono-Medium.ttf
    fonts/IBMPlexMono-SemiBold.ttf
    fonts/IBM-Plex-OFL.txt
)
# Zero-copy display (ADR 0027): the platform implementation of makePlatformDisplay(); Windows has its own (WIN32 block below).
if(NOT WIN32)
  target_sources(uzume_ui PRIVATE gpudisplay_platform_stub.cpp)
endif()
target_include_directories(uzume_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uzume_ui PUBLIC uzume_core uzume_input uzume_audioutil uzume_media uzume_emulation uzume_plugins uzume_formats uzume_mcp Qt6::Quick Qt6::QuickControls2 Qt6::Multimedia)
uzume_warnings(uzume_ui)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Zero-copy display (ADR 0027), Windows platform factory and D3D11 texture source.
if(WIN32)
  target_sources(uzume_ui PRIVATE gpudisplay_platform_win.cpp)
  target_link_libraries(uzume_ui PRIVATE d3d11 dxgi)
endif()
