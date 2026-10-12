if(COPY_BUILD)
	if(DEFINED SkyrimPath)
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "${SkyrimPath}/SKSE/Plugins"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}> "${SkyrimPath}/SKSE/Plugins/"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:${PROJECT_NAME}> "${SkyrimPath}/SKSE/Plugins/"
			VERBATIM)
	else()
		message(WARNING "SkyrimPath is not defined; skipping the post-build game copy.")
	endif()
endif()

if(BUILD_ZIP)
	set(stage_dir "${CMAKE_CURRENT_BINARY_DIR}/package/$<CONFIG>")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${stage_dir}/SKSE/Plugins"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}> "${stage_dir}/SKSE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:${PROJECT_NAME}> "${stage_dir}/SKSE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${stage_dir}/LICENSE"
		COMMAND ${CMAKE_COMMAND} -E chdir "${stage_dir}" ${CMAKE_COMMAND} -E tar cf
			"${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}_${PROJECT_VERSION}_$<CONFIG>.7z" --format=7zip -- SKSE LICENSE
		VERBATIM)
endif()
