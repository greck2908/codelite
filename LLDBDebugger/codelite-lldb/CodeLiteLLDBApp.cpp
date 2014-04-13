#include "CodeLiteLLDBApp.h"
#include <iostream>
#include <wx/sckaddr.h>
#include "LLDBProtocol/LLDBReply.h"
#include "LLDBProtocol/cl_socket_server.h"
#include "LLDBProtocol/LLDBEnums.h"
#include "clcommandlineparser.h"
#include <lldb/API/SBBreakpointLocation.h>
#include <lldb/API/SBFileSpec.h>
#include <lldb/API/SBCommandReturnObject.h>
#include <lldb/API/SBCommandInterpreter.h>
#include <lldb/API/SBFrame.h>
#include <wx/socket.h>
#include "LLDBProtocol/LLDBLocalVariable.h"
#include <wx/msgqueue.h>
#include <wx/wxcrtvararg.h>

//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

static char** _wxArrayStringToCharPtrPtr(const wxArrayString &arr)
{
    char** argv = new char*[arr.size()+1]; // for the NULL
    for(size_t i=0; i<arr.size(); ++i) {
        argv[i] = strdup(arr.Item(i).mb_str(wxConvUTF8).data());
    }
    argv[arr.size()] = NULL;
    return argv;
}

static void DELETE_CHAR_PTR_PTR(char** argv)
{
    size_t i=0;
    while ( argv[i] ) {
        delete [] argv[i];
        ++i;
    }
    delete [] argv;
}

//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

#define CHECK_DEBUG_SESSION_RUNNING() if ( !IsDebugSessionInProgress() ) return

CodeLiteLLDBApp::CodeLiteLLDBApp(const wxString& socketPath)
    : m_networkThread(NULL)
    , m_lldbProcessEventThread(NULL)
    , m_debuggeePid(wxNOT_FOUND)
    , m_interruptReason(kInterruptReasonNone)
    , m_debuggerSocketPath(socketPath)
    , m_exitMainLoop(false)
{
    wxSocketBase::Initialize();
    lldb::SBDebugger::Initialize();
    m_debugger = lldb::SBDebugger::Create();
    wxPrintf("codelite-lldb: lldb initialized successfully\n");
    
    // register our summary
    lldb::SBCommandReturnObject ret;
    m_debugger.GetCommandInterpreter().HandleCommand("type summary add wxString --summary-string \"${var.m_impl._M_dataplus._M_p}\"" , ret);
    m_debugger.GetCommandInterpreter().HandleCommand("type summary add wxPoint --summary-string \"x = ${var.x}, y = ${var.y}\"" , ret);
    m_debugger.GetCommandInterpreter().HandleCommand("type summary add wxRect --summary-string \"(x = ${var.x}, y = ${var.y}) (width = ${var.width}, height = ${var.height})\"" , ret);
    OnInit();
}

CodeLiteLLDBApp::~CodeLiteLLDBApp()
{
    wxDELETE( m_networkThread );
    wxDELETE( m_lldbProcessEventThread );
    m_replySocket.reset(NULL);
    OnExit();
}

int CodeLiteLLDBApp::OnExit()
{
    if ( m_debugger.IsValid() ) {
        lldb::SBDebugger::Destroy( m_debugger );
    }
    lldb::SBDebugger::Terminate();
    return TRUE;
}

bool CodeLiteLLDBApp::OnInit()
{
    wxPrintf("codelite-lldb: starting server on %s\n", m_debuggerSocketPath);
    try {
        m_acceptSocket.CreateServer(m_debuggerSocketPath.mb_str(wxConvUTF8).data());
        
    } catch (clSocketException &e) {
        wxPrintf("codelite-lldb: failed to create server on %s. %s\n", m_debuggerSocketPath, strerror(errno));
        return false;
    }
}

void CodeLiteLLDBApp::StartDebugger(const LLDBCommand& command)
{
    wxPrintf("codelite-lldb: StartDebugger Called\n");
    
    if ( IsDebugSessionInProgress() ) {
        wxPrintf("codelite-lldb: another session is already in progress\n");
        return;
    }
    if ( !command.GetWorkingDirectory().IsEmpty() ) {
        ::wxSetWorkingDirectory( command.GetWorkingDirectory() );
    }
    wxPrintf("codelite-lldb: working directory is set to %s\n", ::wxGetCwd());
#ifdef __WXMAC__
    // On OSX, debugserver executable must exists otherwise lldb will not work properly
    // we ensure that it exists by checking the environment variable LLDB_DEBUGSERVER_PATH
    wxString lldbDebugServer;
    if ( !::wxGetEnv("LLDB_DEBUGSERVER_PATH", &lldbDebugServer) || !wxFileName::Exists(lldbDebugServer) ) {
        wxPrintf("codelite-lldb: LLDB_DEBUGSERVER_PATH environment does not exist or contains a path to a non existent file\n");
        Cleanup();
        return;
    }
#endif

    m_debuggeePid = wxNOT_FOUND;
    m_debugger = lldb::SBDebugger::Create();
    m_target = m_debugger.CreateTarget(command.GetExecutable().mb_str().data());
    m_debugger.SetAsync(true);

    wxPrintf("codelite-lldb: created target for %s\n", command.GetExecutable());
    
    // Launch the thread that will handle the LLDB process events
    m_lldbProcessEventThread = new LLDBProcessEventHandlerThread(this, m_debugger.GetListener(), m_target.GetProcess());
    m_lldbProcessEventThread->Start();

    // In any case, reset the interrupt reason
    m_interruptReason = kInterruptReasonNone;

    // Notify codelite that the debugger started successfully
    NotifyStarted();
}

void CodeLiteLLDBApp::NotifyAllBreakpointsDeleted()
{
    LLDBReply reply;
    reply.SetReplyType( kReplyTypeAllBreakpointsDeleted );
    SendReply( reply );
}

void CodeLiteLLDBApp::NotifyBreakpointsUpdated()
{
    LLDBBreakpoint::Vec_t breakpoints;
    int num = m_target.GetNumBreakpoints();
    wxPrintf("codelite-lldb: Calling NotifyBreakpointsUpdated(). Got %d breakpoints\n", num);
    for(int i=0; i<num; ++i) {
        lldb::SBBreakpoint bp = m_target.GetBreakpointAtIndex(i);
        if ( bp.IsValid() ) {
            
            // Add the parent breakpoint
            LLDBBreakpoint::Ptr_t mainBreakpoint( new LLDBBreakpoint() );
            mainBreakpoint->SetId( bp.GetID() );
            if ( bp.GetNumLocations() >  1 ) {
                
                // add all the children locations to the main breakpoint
                for(size_t i=0; i<bp.GetNumLocations(); ++i) {
                    lldb::SBBreakpointLocation loc = bp.GetLocationAtIndex(i);
                    
                    lldb::SBFileSpec fileLoc = loc.GetAddress().GetLineEntry().GetFileSpec();
                    wxFileName bpFile( fileLoc.GetDirectory(), fileLoc.GetFilename() );

                    // Create a breakpoint for this location
                    LLDBBreakpoint::Ptr_t new_bp(new LLDBBreakpoint());
                    new_bp->SetType( LLDBBreakpoint::kLocation );
                    new_bp->SetFilename( bpFile.GetFullPath() );
                    new_bp->SetLineNumber( loc.GetAddress().GetLineEntry().GetLine() );
                    new_bp->SetName( loc.GetAddress().GetFunction().GetName() );
                    mainBreakpoint->GetChildren().push_back( new_bp );
            }
                
            } else {
                lldb::SBBreakpointLocation loc = bp.GetLocationAtIndex(0);
                lldb::SBFileSpec fileLoc = loc.GetAddress().GetLineEntry().GetFileSpec();
                wxFileName bpFile( fileLoc.GetDirectory(), fileLoc.GetFilename() );
                
                mainBreakpoint->SetType( LLDBBreakpoint::kFileLine );
                mainBreakpoint->SetName( loc.GetAddress().GetFunction().GetName() );
                mainBreakpoint->SetFilename( bpFile.GetFullPath() );
                mainBreakpoint->SetLineNumber( loc.GetAddress().GetLineEntry().GetLine() );
                
            }
            breakpoints.push_back( mainBreakpoint );
        }
    }
    
    LLDBReply reply;
    reply.SetReplyType( kReplyTypeBreakpointsUpdated );
    reply.SetBreakpoints( breakpoints );
    SendReply( reply );
}

void CodeLiteLLDBApp::NotifyExited()
{
    wxPrintf("codelite-lldb: NotifyExited called\n");
    LLDBReply reply;
    reply.SetReplyType( kReplyTypeDebuggerExited );
    SendReply( reply );
    Cleanup();
    m_exitMainLoop = true;
}

void CodeLiteLLDBApp::NotifyRunning()
{
    m_variables.clear();
    LLDBReply reply;
    reply.SetReplyType( kReplyTypeDebuggerRunning );
    SendReply( reply );
}

void CodeLiteLLDBApp::NotifyStarted()
{
    m_variables.clear();
    LLDBReply reply;
    reply.SetReplyType( kReplyTypeDebuggerStartedSuccessfully );
    SendReply( reply );
}

void CodeLiteLLDBApp::NotifyStopped()
{
    m_variables.clear();
    LLDBReply reply;
    wxPrintf("codelite-lldb: NotifyStopped() called. m_interruptReason=%d\n", (int)m_interruptReason);
    reply.SetReplyType( kReplyTypeDebuggerStopped );
    reply.SetInterruptResaon( m_interruptReason );
    
    lldb::SBThread thread = m_target.GetProcess().GetSelectedThread();
    LLDBBacktrace bt( thread );
    reply.SetBacktrace( bt );
    
    // set the selected frame file:line
    if ( thread.IsValid() && thread.GetSelectedFrame().IsValid() ) {
        lldb::SBFrame frame = thread.GetSelectedFrame();
        lldb::SBLineEntry lineEntry = thread.GetSelectedFrame().GetLineEntry();
        if ( lineEntry.IsValid() ) {
            reply.SetLine(lineEntry.GetLine());
            lldb::SBFileSpec fileSepc = frame.GetLineEntry().GetFileSpec();
            reply.SetFilename( wxFileName(fileSepc.GetDirectory(), fileSepc.GetFilename()).GetFullPath() );
        }
    }
    SendReply( reply );

    // reset the interrupt reason
    m_interruptReason = kInterruptReasonNone;
}

void CodeLiteLLDBApp::NotifyStoppedOnFirstEntry()
{
    m_variables.clear();
    LLDBReply reply;
    reply.SetReplyType( kReplyTypeDebuggerStoppedOnFirstEntry );
    SendReply( reply );
}

void CodeLiteLLDBApp::SendReply(const LLDBReply& reply)
{
    try {
        m_replySocket->WriteMessage( reply.ToJSON().format() );

    } catch (clSocketException &e) {
        wxPrintf("codelite-lldb: failed to send reply. %s. %s.\n", e.what().c_str(), strerror(errno));
    }
}

void CodeLiteLLDBApp::RunDebugger(const LLDBCommand& command)
{
    if ( m_debuggeePid != wxNOT_FOUND) {
        wxPrintf("codelite-lldb: another session is already in progress\n");
        return;
    }
    
    if ( m_debugger.IsValid() ) {
        m_variables.clear();
        // Construct char** arrays
        clCommandLineParser parser(command.GetCommandArguments());
        const char** argv = (const char**)_wxArrayStringToCharPtrPtr(parser.ToArray());

        std::string tty_c;
        if ( !command.GetRedirectTTY().IsEmpty() ) {
            tty_c = command.GetRedirectTTY().mb_str(wxConvUTF8).data();
        }
        const char *ptty = tty_c.empty() ? NULL : tty_c.c_str();

        lldb::SBError error;
        lldb::SBListener listener = m_debugger.GetListener();
        lldb::SBProcess process = m_target.Launch(
                                      listener,
                                      argv,
                                      NULL,
                                      ptty,
                                      ptty,
                                      ptty,
                                      NULL,
                                      lldb::eLaunchFlagLaunchInSeparateProcessGroup|lldb::eLaunchFlagStopAtEntry,
                                      true,
                                      error);

        //bool isOk = m_target.LaunchSimple(argv, envp, wd).IsValid();
        DELETE_CHAR_PTR_PTR( const_cast<char**>(argv) );

        if ( !process.IsValid() ) {
            NotifyExited();

        } else {
            m_debuggeePid = process.GetProcessID();
            NotifyRunning();
        }
    }
}

void CodeLiteLLDBApp::Cleanup()
{
    wxPrintf("codelite-lldb: Cleanup() called...\n");
    m_variables.clear();
    wxDELETE( m_networkThread );
    wxDELETE( m_lldbProcessEventThread );
    
    m_interruptReason = kInterruptReasonNone;
    m_debuggeePid = wxNOT_FOUND;
    
    if ( m_target.IsValid() ) {
        m_target.DeleteAllBreakpoints();
        m_target.DeleteAllWatchpoints();
        m_debugger.DeleteTarget( m_target );
    }
    wxPrintf("codelite-lldb: Cleanup() called... done\n");
}

void CodeLiteLLDBApp::ApplyBreakpoints(const LLDBCommand& command)
{
    wxPrintf("codelite-lldb: ApplyBreakpoints called\n");
    if ( m_target.GetProcess().GetState() == lldb::eStateStopped ) {
        wxPrintf("codelite-lldb: ApplyBreakpoints: process state is stopped - will apply them now\n");
        // we can apply the breakpoints
        // Apply all breakpoints with an-invalid breakpoint ID
        LLDBBreakpoint::Vec_t breakpoints = command.GetBreakpoints();
        while( !breakpoints.empty() ) {
            LLDBBreakpoint::Ptr_t breakPoint = breakpoints.at(0);
            if ( !breakPoint->IsApplied() ) {
                switch( breakPoint->GetType() ) {
                case LLDBBreakpoint::kFunction: {
                    wxPrintf("codelite-lldb: creating breakpoint by name: %s\n", breakPoint->GetName());
                    m_target.BreakpointCreateByName(breakPoint->GetName().mb_str().data(), NULL);
                    break;
                }
                case LLDBBreakpoint::kFileLine: {
                    wxPrintf("codelite-lldb: creating breakpoint by location: %s,%d\n", breakPoint->GetFilename(), breakPoint->GetLineNumber());
                    m_target.BreakpointCreateByLocation(breakPoint->GetFilename().mb_str().data(), breakPoint->GetLineNumber());
                    break;
                }
                }
            }
            breakpoints.erase(breakpoints.begin());
        }
        NotifyBreakpointsUpdated();

    } else {
        wxPrintf("codelite-lldb: ApplyBreakpoints: process state is _NOT_ Stopped - interrupting process\n");
        // interrupt the process
        m_interruptReason = kInterruptReasonApplyBreakpoints;
        m_target.GetProcess().SendAsyncInterrupt();
    }
}

void CodeLiteLLDBApp::Continue(const LLDBCommand& command)
{
    CHECK_DEBUG_SESSION_RUNNING();
    wxUnusedVar( command );
    m_target.GetProcess().Continue();
}

void CodeLiteLLDBApp::StopDebugger(const LLDBCommand& command)
{
    CHECK_DEBUG_SESSION_RUNNING();
    NotifyExited();
    Cleanup();
}

void CodeLiteLLDBApp::DeleteAllBreakpoints(const LLDBCommand& command)
{
    CHECK_DEBUG_SESSION_RUNNING();
    wxUnusedVar(command);
    if ( m_target.GetProcess().GetState() == lldb::eStateStopped ) {
        m_target.DeleteAllBreakpoints();
        NotifyAllBreakpointsDeleted();
        
    } else {
        m_interruptReason = kInterruptReasonDeleteAllBreakpoints;
        m_target.GetProcess().SendAsyncInterrupt();
    }
}

void CodeLiteLLDBApp::DeleteBreakpoints(const LLDBCommand& command)
{
    CHECK_DEBUG_SESSION_RUNNING();
    
    
    const LLDBBreakpoint::Vec_t& bps = command.GetBreakpoints();
    if ( bps.empty() ) {
        return;
    }
    
    wxPrintf("codelite-lldb: DeleteBreakpoints called\n");
    if ( m_target.GetProcess().GetState() == lldb::eStateStopped ) {
        wxPrintf("codelite-lldb: DeleteBreakpoints: process state is Stopped - will apply them now\n");
        for(size_t i=0; i<bps.size(); ++i) {
            LLDBBreakpoint::Ptr_t breakpoint = bps.at(i);
            wxPrintf("codelite-lldb: deleting breakpoint: %s\n", breakpoint->ToString());
            if ( breakpoint->IsApplied() ) {
                lldb::SBBreakpoint lldbBreakpoint = m_target.FindBreakpointByID(breakpoint->GetId());
                if ( lldbBreakpoint.IsValid() ) {
                    lldbBreakpoint.ClearAllBreakpointSites();
                    m_target.BreakpointDelete( lldbBreakpoint.GetID() );
                }
            }
        }
        NotifyBreakpointsUpdated();

    } else {
        wxPrintf("codelite-lldb: DeleteBreakpoints: process is Busy - will interrupt it\n");
        m_interruptReason = kInterruptReasonDeleteBreakpoint;
        m_target.GetProcess().SendAsyncInterrupt();
    }
}

void CodeLiteLLDBApp::Next(const LLDBCommand& command)
{
    CHECK_DEBUG_SESSION_RUNNING();
    lldb::SBCommandReturnObject ret;
    m_debugger.GetCommandInterpreter().HandleCommand("next", ret);
    wxUnusedVar( ret );
}

void CodeLiteLLDBApp::StepIn(const LLDBCommand& command)
{
    CHECK_DEBUG_SESSION_RUNNING();
    lldb::SBCommandReturnObject ret;
    m_debugger.GetCommandInterpreter().HandleCommand("step", ret);
    wxUnusedVar( ret );
}

void CodeLiteLLDBApp::StepOut(const LLDBCommand& command)
{
    CHECK_DEBUG_SESSION_RUNNING();
    lldb::SBCommandReturnObject ret;
    m_debugger.GetCommandInterpreter().HandleCommand("finish", ret);
    wxUnusedVar( ret );
}

bool CodeLiteLLDBApp::CanInteract()
{
    return IsDebugSessionInProgress() && (m_target.GetProcess().GetState() == lldb::eStateStopped);
}

bool CodeLiteLLDBApp::IsDebugSessionInProgress()
{
    return m_debugger.IsValid() && m_target.IsValid();
}

void CodeLiteLLDBApp::Interrupt(const LLDBCommand& command)
{
    wxPrintf("codelite-lldb: interrupting debugee process\n");
    m_interruptReason = (eInterruptReason)command.GetInterruptReason();
    m_target.GetProcess().SendAsyncInterrupt();
}

void CodeLiteLLDBApp::AcceptNewConnection() throw(clSocketException)
{
    m_replySocket.reset( NULL );
    wxPrintf("codelite-lldb: waiting for new connection\n");
    try {
        while( true ) {
            m_replySocket = m_acceptSocket.WaitForNewConnection(1);
            if ( m_replySocket ) {
                break;
            }
        }

        // handle the connection to the thread
        m_networkThread = new LLDBNetworkServerThread(this, m_replySocket->GetSocket());
        m_networkThread->Start();

    } catch (clSocketException &e) {
        wxPrintf("codelite-lldb: an error occured while waiting for connection. %s\n", e.what().c_str());
        Cleanup();
        
        // exit
        throw clSocketException("Failed to accept new connection");
    }
}

void CodeLiteLLDBApp::LocalVariables(const LLDBCommand& command)
{
    wxUnusedVar( command );
    LLDBLocalVariable::Vect_t locals;
    
    wxPrintf("codelite-lldb: fetching local variables for selected frame\n");
    lldb::SBFrame frame = m_target.GetProcess().GetSelectedThread().GetSelectedFrame();
    if ( !frame.IsValid() ) {
        NotifyLocals(locals);
    }

    // get list of locals
    lldb::SBValueList args = frame.GetVariables(true, true, false, true);
    for(size_t i=0; i<args.GetSize(); ++i) {
        lldb::SBValue value = args.GetValueAtIndex(i);
        if ( value.IsValid() ) {
            LLDBLocalVariable::Ptr_t var( new LLDBLocalVariable(value) );
            m_variables.insert( std::make_pair(value.GetID(), value) );
            locals.push_back( var );
        }
    }
    NotifyLocals( locals );
}

void CodeLiteLLDBApp::NotifyLocals(LLDBLocalVariable::Vect_t locals)
{
    wxPrintf("codelite-lldb: NotifyLocals called. with %d locals\n", (int)locals.size());
    LLDBReply reply;
    reply.SetReplyType( kReplyTypeLocalsUpdated );
    reply.SetLocals( locals );
    SendReply( reply );
}

// we need to return list of children for a variable
// we stashed the variables we got so far inside a map
void CodeLiteLLDBApp::ExpandVariable(const LLDBCommand& command)
{
    wxPrintf("codelite-lldb: ExpandVariable called for variableId=%d\n", command.GetLldbId());
    int variableId = command.GetLldbId();
    if ( variableId == wxNOT_FOUND ) {
        return;
    }
    
    wxPrintf("codelite-lldb: ExpandVariable called for variableId=%d\n", variableId);
    static const int MAX_ARRAY_SIZE = 50;
    
    LLDBLocalVariable::Vect_t children;
    std::map<int, lldb::SBValue>::iterator iter = m_variables.find(variableId);
    if ( iter != m_variables.end() ) {
        lldb::SBValue value = iter->second;
        int size = value.GetNumChildren();
        
        lldb::TypeClass typeClass = value.GetType().GetTypeClass();
        if ( typeClass == lldb::eTypeClassArray ) {
            size > MAX_ARRAY_SIZE ? size = MAX_ARRAY_SIZE : size = size;
            wxPrintf("codelite-lldb: value %s is an array. Limiting its size\n", value.GetName());
        }
        
        for(int i=0; i<size; ++i) {
            lldb::SBValue child = value.GetChildAtIndex(i);
            if ( child.IsValid() ) {
                LLDBLocalVariable::Ptr_t var( new LLDBLocalVariable(child) );
                children.push_back( var );
                m_variables.insert( std::make_pair(child.GetID(), child) );
            }
        }
        
        LLDBReply reply;
        reply.SetReplyType( kReplyTypeVariableExpanded );
        reply.SetLocals( children );
        reply.SetLldbId( variableId );
        SendReply( reply );
    }
}

void CodeLiteLLDBApp::CallAfter(CodeLiteLLDBApp::CommandFunc_t func, const LLDBCommand& command)
{
    m_commands_queue.Post( std::make_pair(func, command) );
}

void CodeLiteLLDBApp::MainLoop()
{
    try {
        AcceptNewConnection();
        // We got both ends connected
        wxPrintf("codelite-lldb: successfully established connection to codelite\n");
        
        while ( !m_exitMainLoop ) {
            CodeLiteLLDBApp::QueueItem_t msg;
            CodeLiteLLDBApp::NotifyFunc_t notify_func = NULL;
            bool got_something = false;
            if ( m_commands_queue.ReceiveTimeout(1, msg ) == wxMSGQUEUE_NO_ERROR ) {
                // Process the command
                CodeLiteLLDBApp::CommandFunc_t pFunc = msg.first;
                LLDBCommand command = msg.second;
                (this->*pFunc)( command );
                
                got_something = true;
            }
            
            if ( m_notify_queue.ReceiveTimeout(1, notify_func) == wxMSGQUEUE_NO_ERROR ) {
                (this->*notify_func)();
                got_something = true;
            }
            
            if ( !got_something ) {
                wxThread::Sleep(10);
            }
        }
        
        wxPrintf("codelite-lldb: terminaing\n");
        
    } catch (clSocketException &e) {
        wxPrintf("codelite-lldb: an error occured during MainLoop(). %s. strerror=%s\n", e.what().c_str(), strerror(errno));
    }
}
