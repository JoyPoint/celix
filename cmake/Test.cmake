# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

include(FindCUnit)

ADD_CUSTOM_TARGET(test)
MACRO(run_test)
    PARSE_ARGUMENTS(TEST "" "" ${ARGN})
    LIST(GET TEST_DEFAULT_ARGS 0 EXEC)
	
	SET(__testTarget test_${EXEC})
	
	make_directory(${PROJECT_BINARY_DIR}/test_results)
		
	add_custom_target(${__testTarget}
		${EXEC} ${EXEC} 
		COMMAND if [ -e ${PROJECT_BINARY_DIR}/test_results/${EXEC}-Results.xml ]\; then xsltproc --path ${CUNIT_SHARE_DIR} ${CUNIT_SHARE_DIR}/CUnit-Run.xsl ${PROJECT_BINARY_DIR}/test_results/${EXEC}-Results.xml > ${EXEC}-Results.html \; fi
		COMMAND if [ -e ${PROJECT_BINARY_DIR}/test_results/${EXEC}-Listing.xml ]\; then xsltproc --path ${CUNIT_SHARE_DIR} ${CUNIT_SHARE_DIR}/CUnit-List.xsl ${PROJECT_BINARY_DIR}/test_results/${EXEC}-Listing.xml > ${EXEC}-Listing.html \; fi
		WORKING_DIRECTORY ${PROJECT_BINARY_DIR}/test_results
	)
	ADD_DEPENDENCIES(test ${__testTarget})
ENDMACRO(run_test)