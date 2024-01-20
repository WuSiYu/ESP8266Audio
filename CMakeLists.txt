file(GLOB_RECURSE LIB_SRCS
    src/*.c
    src/*.cpp
)

idf_component_register(SRCS ${LIB_SRCS}
                      INCLUDE_DIRS "src"
                      REQUIRES arduino
)
