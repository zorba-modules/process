/*
 * Copyright 2006-2013 The FLWOR Foundation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sstream>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <vector>
#include <iostream>
#include <limits.h>
#include <algorithm>

#ifdef WIN32
#  include <windows.h>
#  ifndef NDEBUG
#    define _CRTDBG_MAP_ALLOC
#    include <stdlib.h>
#    include <crtdbg.h>
#   endif
#else
#  include <unistd.h>
#  ifdef __APPLE__
#    include <sys/wait.h>
#  else
#    include <wait.h>
#  endif
#endif

#include <zorba/item_factory.h>
#include <zorba/singleton_item_sequence.h>
#include <zorba/diagnostic_list.h>
#include <zorba/user_exception.h>
#include <zorba/empty_sequence.h>

#include "process.h"

// Provde the execvpe() function since some platforms don't have it
#ifndef WIN32
int execvpe(const char *program, char **argv, char **envp)
{
  extern char **environ;
  char **saved = environ;
  environ = envp;
  int rc = execvp(program, argv);
  environ = saved;
  return rc;
}
#endif


namespace zorba {
namespace processmodule {

/******************************************************************************
 *****************************************************************************/
void create_result_object(
    zorba::Item&        aResult,
    const std::string&  aStandardOut,
    const std::string&  aErrorOut,
    int                 aExitCode,
    zorba::ItemFactory* aFactory)
{  
  std::vector<std::pair<zorba::Item,zorba::Item> > pairs;
  
  pairs.push_back(std::pair<zorba::Item,zorba::Item>(aFactory->createString("exit-code"), aFactory->createInt(aExitCode)));
  pairs.push_back(std::pair<zorba::Item,zorba::Item>(aFactory->createString("stdout"), aFactory->createString(aStandardOut)));
  pairs.push_back(std::pair<zorba::Item,zorba::Item>(aFactory->createString("stderr"), aFactory->createString(aErrorOut)));
  
  aResult = aFactory->createJSONObject(pairs);  
}

void free_char_vector(std::vector<char*> argv)
{
  for (unsigned int i=0; i<argv.size(); i++)
    free(argv[i]);    
}

#ifdef WIN32

/***********************************************
*  throw a descriptive message of the last error
*  accessible with GetLastError() on windows
*/
void throw_last_error(const zorba::String& aFilename, unsigned int aLineNumber){
  LPVOID lpvMessageBuffer;
  TCHAR lErrorBuffer[512];
  FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
          NULL, GetLastError(),
          MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
          (LPTSTR)&lpvMessageBuffer, 0, NULL);
  wsprintf(lErrorBuffer,TEXT("Process Error Code: %d - Message= %s"),GetLastError(), (TCHAR *)lpvMessageBuffer);
  LocalFree(lpvMessageBuffer);
  Item lQName = ProcessModule::getItemFactory()->createQName(
    "http://zorba.io/modules/process", "COMMUNICATION");
#ifdef UNICODE
  char error_str[1024];
  WideCharToMultiByte(CP_UTF8, 0, lErrorBuffer, -1, error_str, sizeof(error_str), NULL, NULL);
  throw USER_EXCEPTION(lQName, error_str);
#else
  throw USER_EXCEPTION(lQName, lErrorBuffer);
#endif
}

/******************************************
*  read output from child process on windows
*/
void read_child_output(HANDLE aOutputPipe, std::ostringstream& aTargetStream)
{
  CHAR lBuffer[256];
  DWORD lBytesRead;
  
  while(TRUE)
  {
    if (
      !ReadFile(aOutputPipe,lBuffer,sizeof(lBuffer),&lBytesRead,NULL) 
      || !lBytesRead
    )
    {
      if (GetLastError() == ERROR_BROKEN_PIPE)
        break; // finished
      else{
      
         // couldn't read from pipe
         throw_last_error(__FILE__, __LINE__);
      }
    }
    
    // remove the windows specific carriage return outputs
   // std::stringstream lTmp;
   // lTmp.write(lBuffer,lBytesRead);
   // std::string lRawString=lTmp.str();
   // std::replace( lRawString.begin(), lRawString.end(), '\r', ' ' );
   // aTargetStream.write(lRawString.c_str(),static_cast<std::streamsize>(lRawString.length()));
    for(DWORD i=0;i<lBytesRead;i++)
    {
      if(lBuffer[i] != '\r')
        aTargetStream << lBuffer[i];
    }
    lBytesRead = 0;
  }
}

/******************************************
*  Create a child process on windows with
*  redirected output
*/
BOOL create_child_process(HANDLE aStdOutputPipe,HANDLE aStdErrorPipe,const std::string& aCommand,PROCESS_INFORMATION& aProcessInformation){
  STARTUPINFO lChildStartupInfo;
  BOOL result=FALSE;
  
  // set the output handles
  FillMemory(&lChildStartupInfo,sizeof(lChildStartupInfo),0);
  lChildStartupInfo.cb = sizeof(lChildStartupInfo);
  GetStartupInfo(&lChildStartupInfo);
  lChildStartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  lChildStartupInfo.wShowWindow = SW_HIDE; // don't show the command window
  lChildStartupInfo.hStdOutput = aStdOutputPipe;
  lChildStartupInfo.hStdError  = aStdErrorPipe;

  // convert from const char* to char*
  size_t length = strlen(aCommand.c_str());
#ifdef UNICODE
  WCHAR *tmpCommand = new WCHAR[length+1];
  MultiByteToWideChar(CP_UTF8, 0, aCommand.c_str(), -1, tmpCommand, length+1);
#else
  char *tmpCommand=new char[length+1];
  strcpy (tmpCommand,aCommand.c_str());
  tmpCommand[length]='\0';
#endif

  try{
  
    // settings for the child process      
    LPCTSTR lApplicationName=NULL;
    LPTSTR lCommandLine=tmpCommand;
    LPSECURITY_ATTRIBUTES lProcessAttributes=NULL;
    LPSECURITY_ATTRIBUTES lThreadAttributes=NULL;
    BOOL lInheritHandles=TRUE; // that's what we want
    DWORD lCreationFlags=CREATE_NEW_CONSOLE;
    LPVOID lEnvironment=NULL;
    LPCTSTR lCurrentDirectory=NULL; // same as main process
    
    // start child
    result=CreateProcess(
          lApplicationName,lCommandLine,lProcessAttributes,
          lThreadAttributes,lInheritHandles,lCreationFlags,
          lEnvironment,lCurrentDirectory,&lChildStartupInfo,
          &aProcessInformation);
          
  }catch(...){
    delete[] tmpCommand;
    tmpCommand=0;
    throw;  
  }
        
  delete[] tmpCommand;
  tmpCommand=0;
        
  return result;
}

/******************************************
*  run a process that executes the aCommand
*  in a new console and reads the output
*/
int run_process(
  const std::string& aCommand,
  std::ostringstream& aTargetOutStream,
  std::ostringstream& aTargetErrStream)
{
  HANDLE lOutRead, lErrRead, lStdOut, lStdErr;
  SECURITY_ATTRIBUTES lSecurityAttributes;
  PROCESS_INFORMATION lChildProcessInfo;
  DWORD exitCode=0;
    
  // prepare security attributes
  lSecurityAttributes.nLength= sizeof(lSecurityAttributes);
  lSecurityAttributes.lpSecurityDescriptor = NULL;
  lSecurityAttributes.bInheritHandle = TRUE;

  // create output pipes
  if(
      !CreatePipe(&lOutRead,&lStdOut,&lSecurityAttributes,1024*1024) // std::cout >> lOutRead
      || !CreatePipe(&lErrRead,&lStdErr,&lSecurityAttributes,1024*1024) // std::cerr >> lErrRead
    ){
    Item lQName = ProcessModule::getItemFactory()->createQName(
      "http://zorba.io/modules/process", "COMMUNICATION");
    throw USER_EXCEPTION(lQName,
      "Couldn't create one of std::cout/std::cerr pipe for child process execution."
    );
  };
  
  //start child process
  BOOL ok = create_child_process(lStdOut,lStdErr,aCommand,lChildProcessInfo);
  if(ok==TRUE)
  {

    // close unneeded handle  
    CloseHandle(lChildProcessInfo.hThread);
    
    // wait for the process to finish
    WaitForSingleObject(lChildProcessInfo.hProcess,INFINITE);
    if (!GetExitCodeProcess(lChildProcessInfo.hProcess, &exitCode))
    {
      std::stringstream lErrorMsg;
      lErrorMsg 
        << "Couldn't get exit code from child process. Executed command: '" << aCommand << "'.";
      Item lQName = ProcessModule::getItemFactory()->createQName(
        "http://zorba.io/modules/process", "COMMUNICATION");
      throw USER_EXCEPTION(lQName, lErrorMsg.str().c_str());
    }
  
    CloseHandle(lChildProcessInfo.hProcess);
    CloseHandle(lStdOut);
    CloseHandle(lStdErr);

    // read child's output
    read_child_output(lOutRead,aTargetOutStream);
    read_child_output(lErrRead,aTargetErrStream);

    // close 
    CloseHandle(lOutRead);
    CloseHandle(lErrRead);

 }else{
    CloseHandle(lStdOut);
    CloseHandle(lStdErr);
    CloseHandle(lOutRead);
    CloseHandle(lErrRead);
  
     // couldn't launch process
     throw_last_error(__FILE__, __LINE__);
  };
  
  
  return exitCode;
}

#else

#define READ  0
#define WRITE 1

pid_t exec_helper(int *infp, int *outfp, int *errfp, const char *command, char* argv[], char* env[])
{
    int p_stdin[2];
    int p_stdout[2];
    int p_stderr[2];
    pid_t pid;

    if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0 || pipe(p_stderr) != 0)
      return -1;

    pid = fork();

    if (pid < 0)
      return pid;
    else if (pid == 0)
    {
      close(p_stdin[WRITE]);
      dup2(p_stdin[READ], 0);   // duplicate stdin

      close(p_stdout[READ]);
      dup2(p_stdout[WRITE], 1); // duplicate stdout

      close(p_stderr[READ]);
      dup2(p_stderr[WRITE], 2); // duplicate stderr

      if (command)
        execl("/bin/sh", "sh", "-c", command, NULL);
      else if (env == NULL)      
        execvp(argv[0], argv);
      else
        execvpe(argv[0], argv, env);      
        
      perror("execl"); // output the result to standard error
      
      // TODO:
      // Currently, if the child process exits with an error, the following happens:
      // -- exit(errno) is called
      // -- static object destruction ocurrs
      // -- Zorba store is destroyed in the child process and this leaks several URIs 
      //    and prints error messages to stderr. An exception is thrown and this overwrites
      //    the exit code of the invoked process.
      //
      // Until a proper solution is found, the child fork() process will call abort(), which
      // will not trigger static object destruction.
            
      abort();
      // exit(errno);
    }

    if (infp == NULL)
      close(p_stdin[WRITE]);
    else
      *infp = p_stdin[WRITE];

    if (outfp == NULL)
      close(p_stdout[READ]);
    else
      *outfp = p_stdout[READ];

    if (errfp == NULL)
      close(p_stderr[READ]);
    else
      *errfp = p_stderr[READ];

    close(p_stdin[READ]);   // We only write to the forks stdin anyway
    close(p_stdout[WRITE]); // and we only read from its stdout
    close(p_stderr[WRITE]); // and we only read from its stderr
        
    return pid;
}

#endif


/******************************************************************************
 *****************************************************************************/
String ExecFunction::getOneStringArgument (const Arguments_t& aArgs, int aPos) const
{
  Item lItem;
  Iterator_t  args_iter = aArgs[aPos]->getIterator();
  args_iter->open();
  args_iter->next(lItem);
  zorba::String lTmpString = lItem.getStringValue();
  args_iter->close();
  return lTmpString;
}

zorba::ItemSequence_t
ExecFunction::evaluate(
  const Arguments_t& aArgs,
  const zorba::StaticContext* aSctx,
  const zorba::DynamicContext* aDctx) const
{
  std::string lCommand;
  std::vector<std::string> lArgs;
  std::vector<std::string> lEnv;
  int exit_code = 0;

  lCommand = getOneStringArgument(aArgs, 0).c_str();

  if (aArgs.size() > 1)
  {
    zorba::Item lArg;
    Iterator_t arg1_iter = aArgs[1]->getIterator();
    arg1_iter->open();
    while (arg1_iter->next(lArg))    
      lArgs.push_back(lArg.getStringValue().c_str());
    arg1_iter->close();
  }
  
  if (aArgs.size() > 2)
  {
    zorba::Item lArg;
    Iterator_t arg1_iter = aArgs[2]->getIterator();
    arg1_iter->open();
    while (arg1_iter->next(lArg))    
      lEnv.push_back(lArg.getStringValue().c_str());
    arg1_iter->close();    
  }

  std::ostringstream lTmp;

#ifdef WIN32
  // execute process command in a new commandline
  // with quotes at the beggining and at the end
  lTmp << "cmd /C \"";
#endif
 
  lTmp << "\"" << lCommand << "\""; //quoted for spaced paths/filenames
  size_t pos=0;
  for (std::vector<std::string>::const_iterator lIter = lArgs.begin();
       lIter != lArgs.end(); ++lIter)
  {
    pos = (*lIter).rfind('\\')+(*lIter).rfind('/');
    if (int(pos)>=0)
      lTmp << " \"" << *lIter << "\"";
    else
      lTmp << " " << *lIter;
  }
#ifdef WIN32
  lTmp << "\"";   // with quotes at the end for commandline
#endif
  
  std::ostringstream lStdout;
  std::ostringstream lStderr;

#ifdef WIN32
  std::string lCommandLineString = lTmp.str();
  int code = run_process(lCommandLineString, lStdout, lStderr);
  
  if (code != 0)
  {
    std::stringstream lErrorMsg;
    lErrorMsg << "Failed to execute the command (" << code << ")";
    Item lQName = ProcessModule::getItemFactory()->createQName(
      "http://zorba.io/modules/process", "COMMUNICATION");
    throw USER_EXCEPTION(lQName, lErrorMsg.str().c_str());
  }
  exit_code = code;

#else //not WIN32

  int outfp;
  int errfp;
  int status;
  pid_t pid;
  
  std::vector<char*> argv(lArgs.size()+2, NULL);
  std::vector<char*> env(lEnv.size()+1, NULL);

  try
  {
    if (theIsExecProgram)
    {
      argv[0] = strdup(lCommand.c_str());
      for (unsigned int i=0; i<lArgs.size(); i++)
        argv[i+1] = strdup(lArgs[i].c_str()); 
      
      for (unsigned int i=0; i<lEnv.size(); i++)
        env[i] = strdup(lEnv[i].c_str());
      
      pid = exec_helper(NULL, &outfp, &errfp, NULL, argv.data(), lEnv.size() ? env.data() : NULL);
    }
    else
    {
      pid = exec_helper(NULL, &outfp, &errfp, lTmp.str().c_str(), argv.data(), NULL);
    }
    
    if ( pid == -1 )
    {
      std::stringstream lErrorMsg;
      lErrorMsg << "Failed to execute the command (" << pid << ")";
      Item lQName = ProcessModule::getItemFactory()->createQName(
            "http://zorba.io/modules/process", "COMMUNICATION");
      throw USER_EXCEPTION(lQName, lErrorMsg.str().c_str());
      return NULL;
    }
  
    char lBuf[PATH_MAX];
    ssize_t length = 0;
    while ( (length=read(outfp, lBuf, PATH_MAX)) > 0 )
    {
      lStdout.write(lBuf, length);
    }

    status = close(outfp);

    while ( (length=read(errfp, lBuf, PATH_MAX)) > 0 )
    {
      lStderr.write(lBuf, length);
    }

    status = close(errfp);

    if ( status < 0 )
    {
      std::stringstream lErrorMsg;
      lErrorMsg << "Failed to close the err stream (" << status << ")";
      Item lQName = ProcessModule::getItemFactory()->createQName(
        "http://zorba.io/modules/process", "COMMUNICATION");
      throw USER_EXCEPTION(lQName, lErrorMsg.str().c_str());
    }

    int  stat = 0;
    
    pid_t w = waitpid(pid, &stat, 0);
    
    if (w == -1) 
    { 
        std::stringstream lErrorMsg;
        lErrorMsg << "Failed to wait for child process ";
        Item lQName = ProcessModule::getItemFactory()->createQName(
          "http://zorba.io/modules/process", "COMMUNICATION");
        throw USER_EXCEPTION(lQName, lErrorMsg.str().c_str());          
    }

    if (WIFEXITED(stat)) 
    {
        //std::cout << " WEXITSTATUS : " << WEXITSTATUS(stat) << std::endl; std::cout.flush();
        exit_code = WEXITSTATUS(stat);
    } 
    else if (WIFSIGNALED(stat)) 
    {
        //std::cout << " WTERMSIG : " << WTERMSIG(stat) << std::endl; std::cout.flush();
        exit_code = 128 + WTERMSIG(stat);
    }
    else if (WIFSTOPPED(stat)) 
    {
        //std::cout << " STOPSIG : " << WSTOPSIG(stat) << std::endl; std::cout.flush();
        exit_code = 128 + WSTOPSIG(stat);
    }
    else
    {
        //std::cout << " else : " << std::endl; std::cout.flush();
        exit_code = 255;
    }
    
    //std::cout << " exit_code : " << exit_code << std::endl; std::cout.flush();
    free_char_vector(argv);
    free_char_vector(env);
  }
  catch (...)
  {
    free_char_vector(argv);
    free_char_vector(env);
    throw;
  }
#endif // WIN32

  zorba::Item lResult;
  create_result_object(lResult, lStdout.str(), lStderr.str(), exit_code,
                       theModule->getItemFactory());  
  return zorba::ItemSequence_t(new zorba::SingletonItemSequence(lResult));
}


/******************************************************************************
 *****************************************************************************/
ProcessModule::~ProcessModule()
{
  for (FuncMap_t::const_iterator lIter = theFunctions.begin();
       lIter != theFunctions.end(); ++lIter) {
    delete lIter->second;
  }
  theFunctions.clear();
}

zorba::ExternalFunction*
ProcessModule::getExternalFunction(const zorba::String& aLocalname)
{
  FuncMap_t::const_iterator lFind = theFunctions.find(aLocalname);
  zorba::ExternalFunction*& lFunc = theFunctions[aLocalname];
  if (lFind == theFunctions.end())
  {
    if (aLocalname.compare("exec-command") == 0)
    {
      lFunc = new ExecFunction(this);
    }
    else if (aLocalname.compare("exec") == 0)
    {
      lFunc = new ExecFunction(this, true);
    }
  }
  return lFunc;
}

void ProcessModule::destroy()
{
  if (!dynamic_cast<ProcessModule*>(this)) {
    return;
  }
  delete this;
}

ItemFactory* ProcessModule::theFactory = 0;

} /* namespace processmodule */
} /* namespace zorba */

#ifdef WIN32
#  define DLL_EXPORT __declspec(dllexport)
#else
#  define DLL_EXPORT __attribute__ ((visibility("default")))
#endif

extern "C" DLL_EXPORT zorba::ExternalModule* createModule() {
  return new zorba::processmodule::ProcessModule();
}
/* vim:set et sw=2 ts=2: */
