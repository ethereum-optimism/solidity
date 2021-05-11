set(
	sources
	CommandLineInterface.cpp CommandLineInterface.h
	main.cpp
)

add_executable(osolc ${sources})
target_link_libraries(osolc PRIVATE solidity Boost::boost Boost::program_options)

include(GNUInstallDirs)
install(TARGETS osolc DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(SOLC_LINK_STATIC AND UNIX AND NOT APPLE)
	# Produce osolc as statically linked binary (includes C/C++ standard libraries)
	# This is not supported on macOS, see
	# https://developer.apple.com/library/content/qa/qa1118/_index.html.
	set_target_properties(
		osolc PROPERTIES
		LINK_FLAGS -static
		LINK_SEARCH_START_STATIC ON
		LINK_SEARCH_END_STATIC ON
	)
endif()