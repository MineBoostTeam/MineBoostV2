set(gui_SRCS
	${CMAKE_CURRENT_SOURCE_DIR}/guiAnimatedImage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiBackgroundImage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiBox.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiButton.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiButtonImage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiButtonItemImage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiChatConsole.cpp
	# MineBoost: ClientChat temporarily disabled -- file kept in the tree
	# for restoring later, just not compiled in. See the comment on the
	# "gui/guiClientChat.h" include in src/client/game.cpp.
	# ${CMAKE_CURRENT_SOURCE_DIR}/guiClientChat.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiEditBox.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiEditBoxWithScrollbar.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiEngine.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiFormSpecMenu.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiInventoryList.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiItemImage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiKeyChangeMenu.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiOpenURL.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiPasswordChange.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiPathSelectMenu.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiScene.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiScrollBar.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiScrollContainer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiTable.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiHyperText.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/guiVolumeChange.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/modalMenu.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/profilergraph.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/touchcontrols.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/touchscreenlayout.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/touchscreeneditor.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Sprite.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/SpriteManager.cpp
	# MineBoost: the old Irrlicht-custom-widget settings menu (Items/
	# Menu/Button/PhotoHudSettingsMenu) has been replaced by the new
	# Dear ImGui-based ImGuiMineBoostMenu below -- these 4 files are kept
	# in the tree purely as reference for whatever hasn't been ported yet
	# (currently just "Move HUD" drag-repositioning, which still lives in
	# Menu.cpp -- see the note on this in ImGuiMineBoostMenu.h), but are
	# no longer built.
	# ${CMAKE_CURRENT_SOURCE_DIR}/custom_menu/Items.cpp
	# ${CMAKE_CURRENT_SOURCE_DIR}/custom_menu/Menu.cpp
	# ${CMAKE_CURRENT_SOURCE_DIR}/custom_menu/Button.cpp
	# ${CMAKE_CURRENT_SOURCE_DIR}/custom_menu/PhotoHudSettingsMenu.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/custom_menu/ImGuiMineBoostMenu.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ImGuiManager.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_texture_bridge.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_hud.cpp
	PARENT_SCOPE
)
