add_library (rai_common
	alarm.cpp
	alarm.hpp
	blocks.cpp
	blocks.hpp
	errors.cpp
	errors.hpp
	numbers.cpp
	numbers.hpp
	util.cpp
	util.hpp
	parameters.cpp
	parameters.hpp
	stat.cpp
	stat.hpp)
target_link_libraries (rai_common
	ed25519
	${CRYPTOPP_LIBRARY})

target_compile_definitions(rai_common
	PUBLIC
		-DACTIVE_NETWORK=${ACTIVE_NETWORK}
)
