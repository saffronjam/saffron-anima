# saffron_compile_shaders(<target> <shader_src_dir> <out_dir>)
#
# Compiles every *.slang in <shader_src_dir> to <out_dir>/<name>.spv with slangc
# (all [shader(...)]-tagged entry points in one module) and makes <target> depend
# on the results so they are built alongside it. Also copies each .slang source next
# to its .spv so the runtime node-graph codegen can splice the übershader (mesh.slang).
function(saffron_compile_shaders TARGET SHADER_DIR OUT_DIR)
    file(GLOB shader_sources CONFIGURE_DEPENDS "${SHADER_DIR}/*.slang")

    set(spv_outputs)
    foreach(shader ${shader_sources})
        get_filename_component(name ${shader} NAME_WE)
        set(out "${OUT_DIR}/${name}.spv")
        set(src_copy "${OUT_DIR}/${name}.slang")
        add_custom_command(
            OUTPUT ${out} ${src_copy}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}
            COMMAND ${SAFFRON_SLANGC} ${shader}
                    -profile glsl_450 -target spirv -emit-spirv-directly
                    -fvk-use-entrypoint-name -matrix-layout-column-major
                    -o ${out}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${shader} ${src_copy}
            DEPENDS ${shader}
            COMMENT "slangc ${name}.slang -> ${name}.spv (+ source copy)"
            VERBATIM)
        list(APPEND spv_outputs ${out} ${src_copy})
    endforeach()

    add_custom_target(${TARGET}_shaders DEPENDS ${spv_outputs})
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()
