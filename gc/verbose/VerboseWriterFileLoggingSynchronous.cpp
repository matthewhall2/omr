/*******************************************************************************
 * Copyright IBM Corp. and others 1991
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "modronapicore.hpp"
#include "VerboseWriterFileLoggingSynchronous.hpp"

#include "EnvironmentBase.hpp"
#include "GCExtensionsBase.hpp"
#include "VerboseManager.hpp"

#include <string.h>

#include "VerboseBuffer.hpp"
#include "VerboseHandlerOutput.hpp"

MM_VerboseWriterFileLoggingSynchronous::MM_VerboseWriterFileLoggingSynchronous(MM_EnvironmentBase *env, MM_VerboseManager *manager)
	:MM_VerboseWriterFileLogging(env, manager, VERBOSE_WRITER_FILE_LOGGING_SYNCHRONOUS)
	,_logFileDescriptor(-1)
{
	/* No implementation */
}

/**
 * Create a new MM_VerboseWriterFileLoggingSynchronous instance.
 * @return Pointer to the new MM_VerboseWriterFileLoggingSynchronous.
 */
MM_VerboseWriterFileLoggingSynchronous *
MM_VerboseWriterFileLoggingSynchronous::newInstance(MM_EnvironmentBase *env, MM_VerboseManager *manager, char *filename, uintptr_t numFiles, uintptr_t numCycles)
{
	MM_GCExtensionsBase *extensions = MM_GCExtensionsBase::getExtensions(env->getOmrVM());
	
	MM_VerboseWriterFileLoggingSynchronous *agent = (MM_VerboseWriterFileLoggingSynchronous *)extensions->getForge()->allocate(sizeof(MM_VerboseWriterFileLoggingSynchronous), OMR::GC::AllocationCategory::DIAGNOSTIC, OMR_GET_CALLSITE());
	if(agent) {
		new(agent) MM_VerboseWriterFileLoggingSynchronous(env, manager);
		if(!agent->initialize(env, filename, numFiles, numCycles)) {
			agent->kill(env);
			agent = NULL;
		}
	}
	return agent;
}

/**
 * Initializes the MM_VerboseWriterFileLoggingSynchronous instance.
 * @return true on success, false otherwise
 */
bool
MM_VerboseWriterFileLoggingSynchronous::initialize(MM_EnvironmentBase *env, const char *filename, uintptr_t numFiles, uintptr_t numCycles)
{
	return MM_VerboseWriterFileLogging::initialize(env, filename, numFiles, numCycles);
}

/**
 * Tear down the structures managed by the MM_VerboseWriterFileLoggingSynchronous.
 * Tears down the verbose buffer.
 */
void
MM_VerboseWriterFileLoggingSynchronous::tearDown(MM_EnvironmentBase *env)
{
	MM_VerboseWriterFileLogging::tearDown(env);
}

/**
 * Opens the file to log output to and prints the header.
 * @return true on sucess, false otherwise
 */
bool
MM_VerboseWriterFileLoggingSynchronous::openFile(MM_EnvironmentBase *env, bool printInitializedHeader)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	MM_GCExtensionsBase* extensions = env->getExtensions();
	const char* version = omrgc_get_version(env->getOmrVM());
	
	char *filenameToOpen = expandFilename(env, _currentFile);
	if (NULL == filenameToOpen) {
		return false;
	}
	
	int32_t openFlags =  EsOpenRead | EsOpenWrite | EsOpenCreate | _manager->fileOpenMode(env);

	_logFileDescriptor = omrfile_open(filenameToOpen, openFlags, 0666);
	if(-1 == _logFileDescriptor) {
		char *cursor = filenameToOpen;
		/**
		 * This may have failed due to directories in the path not being available.
		 * Try to create these directories and attempt to open again before failing.
		 */
		while ( (cursor = strchr(++cursor, DIR_SEPARATOR)) != NULL ) {
			*cursor = '\0';
			omrfile_mkdir(filenameToOpen);
			*cursor = DIR_SEPARATOR;
		}

		/* Try again */
		_logFileDescriptor = omrfile_open(filenameToOpen, openFlags, 0666);
		if (-1 == _logFileDescriptor) {
			_manager->handleFileOpenError(env, filenameToOpen);
			extensions->getForge()->free(filenameToOpen);
			return false;
		}
	}

	extensions->getForge()->free(filenameToOpen);
	
	omrfile_printf(_logFileDescriptor, getHeader(env), version);
	/* Print an Initialized Stanza in new file */
	if (printInitializedHeader) {
		MM_VerboseBuffer* buffer = MM_VerboseBuffer::newInstance(env, INITIAL_BUFFER_SIZE);
		if (NULL != buffer) {
			_manager->getVerboseHandlerOutput()->outputInitializedStanza(env, buffer);
			outputString(env, buffer->contents());
			buffer->kill(env);
		}
	}

	return true;
}

/**
 * Prints the footer and closes the file being logged to.
 */
void
MM_VerboseWriterFileLoggingSynchronous::closeFile(MM_EnvironmentBase *env)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	
	if(-1 != _logFileDescriptor) {
		omrfile_write_text(_logFileDescriptor, getFooter(env), strlen(getFooter(env)));
		omrfile_write_text(_logFileDescriptor, "\n", strlen("\n"));
		omrfile_close(_logFileDescriptor);
		_logFileDescriptor = -1;
	}
}

void
MM_VerboseWriterFileLoggingSynchronous::outputString(MM_EnvironmentBase *env, const char* string)
{
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());

	if(-1 == _logFileDescriptor) {
		/**
		 * Under normal circumstances, new file should be opened during endOfCycle call.
		 * This path works as one backup, in case we failed to open the file,  we'll attempt to open it again before outputting the string.
		 */
		openFile(env);
	}

	if(-1 != _logFileDescriptor){
		omrfile_write_text(_logFileDescriptor, string, strlen(string));
	} else {
		omrfile_write_text(OMRPORT_TTY_ERR, string, strlen(string));
	}
}
