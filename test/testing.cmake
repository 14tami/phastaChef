macro(cp_parallel_test name procs dir exe)
  set(tname chefphasta_${name})
  add_test(
    NAME ${tname}
    COMMAND ${MPIRUN} ${MPIRUN_PROCFLAG} ${procs} ${exe} ${ARGN}
    WORKING_DIRECTORY ${dir} )
  set_tests_properties(${tname} PROPERTIES LABELS "chefphasta")
endmacro(cp_parallel_test)

macro(cp_serial_test name exe)
  set(tname chefphasta_${name})
  add_test( NAME ${tname} COMMAND ${exe} ${ARGN} )
  set_tests_properties(${tname} PROPERTIES LABELS "chefphasta")
endmacro(cp_serial_test)

macro(cp_move_dir name work src) 
  set(mname chefphasta_${name})
  set(tname ${mname}_mv${src})
  set(tgtdir ${work}/${src}_${mname}) 
  add_test(
    NAME ${tname}
    COMMAND rm -rf ${tgtdir} && mv ${work}/${src} ${tgtdir}
    WORKING_DIRECTORY ${work})
  set_tests_properties(${tname} PROPERTIES LABELS "chefphasta")
endmacro(cp_move_dir)

set(CDIR ${CASES}/incompressible)

set(exe ${PHASTACHEF_BINARY_DIR}/chefPhasta_posix)
set(casename posix_incompressible)
cp_serial_test(copy_${casename}
  cp ${PHASTA_SOURCE_DIR}/phSolver/common/input.config ${CDIR})
cp_parallel_test(${casename} 4 ${CDIR} ${exe})
cp_parallel_test(check_${casename} 4 ${CDIR}
  ${PHASTA_BINARY_DIR}/bin/checkphasta
  ${CDIR}/4-procs_case/
  ${CDIR}/4-procs_case-SyncIO-2_ref/
  2 1e-6)
cp_move_dir(${casename} ${CDIR} 4-procs_case)

set(exe ${PHASTACHEF_BINARY_DIR}/chefPhasta_stream)
set(casename stream_incompressible)
cp_parallel_test(${casename} 4 ${CDIR} ${exe})
cp_parallel_test(check_${casename} 4 ${CDIR} 
  ${PHASTA_BINARY_DIR}/bin/checkphasta
  ${CDIR}/4-procs_case/
  ${CDIR}/4-procs_case-SyncIO-2_ref/
  2 1e-6)
cp_move_dir(${casename} ${CDIR} 4-procs_case)

set(exe ${PHASTACHEF_BINARY_DIR}/chefPhastaLoop_stream)
set(maxTimeStep 8)
set(casename loopStream_incompressible)
cp_parallel_test(${casename} 4 ${CDIR} ${exe} ${maxTimeStep})
cp_move_dir(${casename} ${CDIR} 4-procs_case)

set(exe ${PHASTACHEF_BINARY_DIR}/chefPhastaLoop_stream_ur)
set(maxTimeStep 12)
set(casename loopStreamUR_incompressible)
cp_parallel_test(${casename} 4 ${CDIR} ${exe} ${maxTimeStep})
cp_move_dir(${casename} ${CDIR} 4-procs_case)
cp_move_dir(${casename} ${CDIR} 4)

if( ${GMI_SIM_FOUND} )
  set(CDIR ${CASES}/incompressibleAdapt)
  set(exe ${PHASTACHEF_BINARY_DIR}/chefPhastaLoop_stream_adapt)
  set(maxTimeStep 8)
  set(casename loopStreamAdapt_incompressible)
  cp_parallel_test(${casename} 2 ${CDIR} ${exe} ${maxTimeStep})
  cp_move_dir(${casename} ${CDIR} 2-procs_case)
  cp_move_dir(${casename} ${CDIR} 4)
  cp_move_dir(${casename} ${CDIR} 8)
endif()
