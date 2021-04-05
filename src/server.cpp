#include "server_config_loader.cpp"
#include "server.h"
#include "md5_hash.cpp"
#include "server_http_parsing.cpp"

// TODO(vincent): Authentication
// https://developer.mozilla.org/en-US/docs/Web/HTTP/Authentication
// TODO(vincent): multisite
// TODO(vincent): linux port


internal initialize_server_memory_result
InitializeServerMemory(server_memory *Memory, platform_work_queue *Queue, 
                       platform_add_entry *PlatformAddEntry)
{
    TestMD5();
    TestFromBase64();
    
    // NOTE(vincent): Initialize server state.
    initialize_server_memory_result InitResult = {};
    
    server_state *State = (server_state *)Memory->Storage;
    InitializeArena(&State->Arena, Memory->StorageSize - sizeof(server_state),
                    (u8 *)Memory->Storage + sizeof(server_state));
    
    State->Queue = Queue;
    State->PlatformAddEntry = PlatformAddEntry;
    
    // NOTE(vincent): Load config file
    parsed_config_file_result *Config = &State->Config;
    Assert(sizeof(DEFAULT_SERVER_PORT) <= ArrayCount(Config->PortString));
    Sprint(Config->PortString, DEFAULT_SERVER_PORT); // initializing to default server port number
    InitResult.ParsingErrorCount = ParseConfigFile(Config);
    InitResult.PortString = Config->PortString;
    
    // NOTE(vincent): Push string constants tightly and null-terminate them.
    // Note that sizeof() on a string literal counts the terminating null character,
    // and that should be a compile-time calculation.
#define STRING_OK "HTTP/1.1 200 OK\r\n\r\n"
#define STRING_BR "HTTP/1.1 400 Bad Request\r\n\r\n"
#define STRING_NF "HTTP/1.1 404 Not Found\r\n\r\n"
#define STRING_UN "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"Access to the staging site\"\r\n\r\n"
#define STRING_FB "HTTP/1.1 403 Forbidden\r\n\r\n"
    State->StringOK = PushArray(&State->Arena, sizeof(STRING_OK), char);
    State->StringBR = PushArray(&State->Arena, sizeof(STRING_BR), char);
    State->StringNF = PushArray(&State->Arena, sizeof(STRING_NF), char);
    State->StringUN = PushArray(&State->Arena, sizeof(STRING_UN), char);
    State->StringFB = PushArray(&State->Arena, sizeof(STRING_FB), char);
    Sprint(State->StringOK, STRING_OK);
    Sprint(State->StringBR, STRING_BR);
    Sprint(State->StringNF, STRING_NF);
    Sprint(State->StringUN, STRING_UN);
    Sprint(State->StringFB, STRING_FB);
    
    // NOTE(vincent): task_with_memory and subarena initialization
    u32 RemainingArenaSize = State->Arena.Size - State->Arena.Used;
    Assert(RemainingArenaSize >= Megabytes(50));
    u32 SubArenaSize = RemainingArenaSize / ArrayCount(State->Tasks);
    for (u32 TaskIndex = 0; TaskIndex < ArrayCount(State->Tasks); TaskIndex++)
    {
        task_with_memory *Task = State->Tasks + TaskIndex;
        Task->BeingUsed = false;
        SubArena(&Task->Arena, &State->Arena, SubArenaSize);
    }
    
    return InitResult;
}

internal task_with_memory *
BeginTaskWithMemory(server_state *State)
{
    // NOTE(vincent): Linear scan for an available task_with_memory in the server state.
    task_with_memory *FoundTask = 0;
    for (u32 TaskIndex = 0; TaskIndex < ArrayCount(State->Tasks); TaskIndex++)
    {
        task_with_memory *Task = State->Tasks + TaskIndex;
        if (Task->BeingUsed == false)
        {
            FoundTask = Task;
            Task->BeingUsed = true;
            Task->TempMemory = BeginTemporaryMemory(&Task->Arena);
            break;
        }
    }
    return FoundTask;
}

inline void
EndTaskWithMemory(task_with_memory *Task)
{
    EndTemporaryMemory(Task->TempMemory);
    //CompletePreviousWritesBeforeFutureWrites; // TODO(vincent): not sure why this would be warranted
    Task->BeingUsed = false;
}

internal string
DecodeAuthString(memory_arena *Arena, string AuthString)
{
    // We want to do the following transformation:
    // base64(username:password) -> username:password -> username:md5(password)
    // where the md5 part is a printable 32-byte ascii version of the md5 hash.
    
    char *Dest = PushArray(Arena, AuthString.Length + 72, char);
    
    ZeroBytes(Dest, AuthString.Length + 72);  
    // TODO(vincent): This seems to fix a bug with MD5 where it produces undesired hashes on
    // subsequent memory reuse, but it shouldn't be necessary since MD5() should be producing the padding
    // for itself. What is happening here?
    
    string Plain = FromBase64(AuthString, Dest);  // this should be less bytes than the source
    
    printf("Plain : ");
    PrintString(Plain);
    printf("\n");
    Assert(StringsAreEqual(Plain, "user:user"));
    
    string PasswordPart = StringSuffixAfter(Plain, ':');
    
    printf("PasswordPart (%d) : ", PasswordPart.Length);
    PrintString(PasswordPart);
    Assert(StringsAreEqual(PasswordPart, "user"));
    printf("\n");
    
    md5_result Hash = MD5((u8 *)PasswordPart.Base, PasswordPart.Length); // requires up to 72 bytes of padding
    
    
    PrintMD5NoNull(PasswordPart.Base, Hash); // overwrites 32 bytes
    
    string DecodedString = StringBaseLength(Dest, Plain.Length - PasswordPart.Length + 32);
    
    
    printf("DecodedString : ");
    PrintString(DecodedString);
    Assert(StringsAreEqual(DecodedString, "user:ee11cbb19052e40b07aac0ca060c23ee"));
    printf("\n");
    
    
    return DecodedString;
}


enum access_result
{
    AccessResult_Unauthorized,
    AccessResult_Forbidden,
    AccessResult_Granted,
};
internal access_result
LoadHtpasswd(memory_arena *Arena, string CompletePath, u32 RootLength, string AuthString)
{
    access_result Result = AccessResult_Granted;
    
    string Scratch = StringBaseLength(PushArray(Arena, CompletePath.Length + 10, char), 0);
    AppendString(&Scratch, CompletePath);
    push_read_entire_file ReadResult = {};
    
    while (!ReadResult.Memory && TruncateStringUntil(&Scratch, '/') && Scratch.Length >= RootLength)
    {
        AppendStringLiteralAndNull(&Scratch, ".htpasswd");
        printf(Scratch.Base);
        printf("\n");
        ReadResult = PushReadEntireFile(Arena, Scratch.Base);
        TruncateStringUntil(&Scratch, '/');
    }
    
    if (ReadResult.Memory)
    {
        printf("PROTECTED\n");
        // NOTE(vincent): File is protected
        // Unauthorized if no auth string given (rule: zero is initialization), forbidden otherwise.
        
        Result = AuthString.Base == 0 ? AccessResult_Unauthorized : AccessResult_Forbidden;
        if (ReadResult.Success && AuthString.Base)
        {
            string DecodedAuthString = DecodeAuthString(Arena, AuthString);
            PrintString(DecodedAuthString);
            printf(" DECODED\n");
            // NOTE(vincent): Compare htpasswd entries with the decoded auth string as you parse the file
            // and see whether there is a match.
            
            b32 InEntry = false;
            string LastEntry;
            LastEntry.Base = ReadResult.Memory;
            LastEntry.Length = 0;
            for (u32 Byte = 0; Byte < ReadResult.Size; Byte++)
            {
                char *C = ReadResult.Memory + Byte;
                if (!InEntry && !IsWhitespace(*C))
                {
                    InEntry = true;
                    LastEntry.Base = C;
                }
                else if (InEntry && IsWhitespace(*C))
                {
                    InEntry = false;
                    LastEntry.Length = (u32)(C - LastEntry.Base);
                    if (StringsAreEqual(LastEntry, DecodedAuthString))
                    {
                        printf("SUCCESSFUL\n");
                        // NOTE(vincent): Successful authentication
                        Result = AccessResult_Granted;
                        break;
                    }
                }
            }
        }
        else
        {
            // TODO(vincent): protected but not properly loaded.
            // Not sure whether we should do anything here.
        }
    }
    else
    {
        printf("NOT PROTECTED\n");
    }
    
    return Result;
}


struct receive_and_send_work
{
    struct sockaddr IncomingAddress;
    SOCKET ClientSocket;
    server_state *State;
    task_with_memory *Task;
};


internal 
PLATFORM_WORK_QUEUE_CALLBACK(ReceiveAndSend)
{
    receive_and_send_work *Work = (receive_and_send_work *)Data;
    SOCKET ClientSocket = Work->ClientSocket;
    struct sockaddr *IncomingAddress = &Work->IncomingAddress;
    
    memory_arena *Arena = &Work->Task->Arena;
    
    char *StringOK = Work->State->StringOK;
    char *StringBR = Work->State->StringBR;
    char *StringNF = Work->State->StringNF;
    char *StringUN = Work->State->StringUN;
    char *StringFB = Work->State->StringFB;
    parsed_config_file_result *Config = &Work->State->Config;
    char *Root = Config->Root;
    
    //server_state *State = (server_state *)Memory->PermanentStorage;
    
    //int SizeTheirAddress = sizeof(*IncomingAddress);
    
    char *AddressString = PushArray(Arena, INET6_ADDRSTRLEN, char);
    inet_ntop(IncomingAddress->sa_family, GetInternetAddress(IncomingAddress),
              AddressString, INET6_ADDRSTRLEN);
    
    u32 PrintBufferSize = 8192;//1024;
    char *PrintBuffer = PushArray(Arena, PrintBufferSize, char);
    string ToPrint;
    ToPrint.Base = PrintBuffer;
    ToPrint.Length = 0;
    
    ToPrint.Length += Sprint(PrintBuffer + ToPrint.Length, "Server: got connection from ");
    ToPrint.Length += Sprint(PrintBuffer + ToPrint.Length, AddressString);
    ToPrint.Length += Sprint(PrintBuffer + ToPrint.Length, " ");
    u32 ReceiveBufferSize = 8192;  // 8*1024 bytes
    
    u32 LengthToSend = 0;
    char *SendBuffer = 0;
    
    for (;;)
    {
        char *ReceiveBuffer = PushArray(Arena, ReceiveBufferSize, char);
        int BytesReceived = recv(ClientSocket, ReceiveBuffer, ReceiveBufferSize, 0);
        
        //if (Result.BytesReceived <= 0 || Result.BytesReceived == INVALID_SOCKET)
        //break;
        if (HandleReceiveError(BytesReceived, ClientSocket))
        {
            // TODO(vincent): make sure this is handled properly on all platforms
            // TODO(vincent): should Windows branch here when BytesReceived == 0 ?
#if 1
            // NOTE(vincent): This is for debugging purposes. Watch out for buffer overflow~
            ToPrint.Length += Sprint(PrintBuffer + ToPrint.Length, "BytesReceived: ");
            ToPrint.Length += SprintInt(PrintBuffer + ToPrint.Length, BytesReceived);
            ToPrint.Length += Sprint(PrintBuffer + ToPrint.Length, "\n\n");
#if 1
            // NOTE(vincent): If you enable this you probably want to increase PrintBufferSize.
            ToPrint.Length += SprintBounded(PrintBuffer + ToPrint.Length, ReceiveBuffer, BytesReceived);
            ToPrint.Length += BinaryToHexadecimal(PrintBuffer + ToPrint.Length, ReceiveBuffer,
                                                  BytesReceived);
            ToPrint.Length += Sprint(PrintBuffer + ToPrint.Length, "\n");
#endif
#endif
            
            http_request Request = ParseHTTPRequest(ReceiveBuffer, BytesReceived);
            if (Request.IsValid)
            {
                printf("Isolated Request AuthString: ");
                PrintString(Request.AuthString);
                printf("\n");
                
                printf("Isolated Host string: ");
                PrintString(Request.Host);
                printf("\n");
                
                // TODO(vincent): use Result.Host somehow
                // TODO(vincent): maybe use Result.HttpVersion?
                
                // NOTE(vincent): Concatenate Root and Request.Path into the arena
                u32 RootLength = StringLength(Root);
                u32 RequestLength = Request.RequestPath.Length;
                u32 CompletePathLength = RootLength + RequestLength;
                string CompletePath = StringBaseLength(PushArray(Arena, CompletePathLength + 1, char),
                                                       CompletePathLength);
                SprintNoNull(CompletePath.Base, Root);
                Sprint(CompletePath.Base + RootLength, Request.RequestPath);
                
                // NOTE(vincent): Check for Htpasswd file and get access result
                access_result AccessResult = 
                    LoadHtpasswd(Arena, CompletePath, RootLength, Request.AuthString);
                
                
                switch (AccessResult)
                {
                    case AccessResult_Unauthorized:
                    {
                        printf("RESULT: UNAUTHORIZED\n");
                        LengthToSend = sizeof(STRING_UN) - 1;
                        SendBuffer = PushArray(Arena, LengthToSend, char);
                        SprintNoNull(SendBuffer, StringUN);
                    } break;
                    case AccessResult_Forbidden:
                    {
                        printf("RESULT: FORBIDDEN\n");
                        LengthToSend = sizeof(STRING_FB) - 1;
                        SendBuffer = PushArray(Arena, LengthToSend, char);
                        SprintNoNull(SendBuffer, StringFB);
                    } break;
                    case AccessResult_Granted:
                    {
                        printf("RESULT: GRANTED\n");
                        SendBuffer = PushArray(Arena, sizeof(STRING_OK)-1, char);
                        SprintNoNull(SendBuffer, StringOK);
                        
                        // NOTE(vincent): Try to load the file
                        u32 ArenaRemainingSize = Arena->Size - Arena->Used;
                        char *Body = PushArray(Arena, ArenaRemainingSize, char);
                        safer_read_file_result ReadFileResult =
                            SaferReadEntireFileInto(Body, CompletePath.Base, ArenaRemainingSize);
                        
                        if (ReadFileResult.Success)
                        {
                            // 200 OK
                            LengthToSend = (sizeof(STRING_OK) - 1) + (u32)ReadFileResult.Size;
                        }
                        else
                        {
                            // 404 Not Found
                            LengthToSend = sizeof(STRING_NF) - 1;
                            SprintNoNull(SendBuffer, StringNF);
                        }
                    } break;
                }
            }
            else
            {
                // 400 Bad Request
                LengthToSend = sizeof(STRING_BR) - 1;
                SendBuffer = PushArray(Arena, LengthToSend, char);
                SprintNoNull(SendBuffer, StringBR);
            }
            
            ToPrint.Length +=
                SprintUntilDelimiter(PrintBuffer + ToPrint.Length, ReceiveBuffer, '\r');
            ToPrint.Length += Sprint(PrintBuffer + ToPrint.Length, "\n");
            break;
        } // END if (HandleReceiveError(BytesReceived))
        else
            break;
    } // END for (;;)
    
    
    
    int BytesSent = send(ClientSocket, SendBuffer, LengthToSend, 0);
    if (HandleSendError(BytesSent, ClientSocket))
    {
        // TODO(vincent): handle this correctly on all platforms
#if 1
        ToPrint.Length += sprintf(PrintBuffer + ToPrint.Length, "BytesSent: %d\n", BytesSent);
#endif
    }
    
    Assert(ToPrint.Length <= PrintBufferSize);
    puts(ToPrint.Base);
    
    // TODO(vincent): handle this on all platforms :)
    ShutdownConnection(ClientSocket);
    
    EndTaskWithMemory(Work->Task);
}

internal void
PrepareHandshaking(server_memory *Memory, struct sockaddr *IncomingAddress, SOCKET ClientSocket, platform_work_queue *Queue)
{
    server_state *State = (server_state *)Memory->Storage;
    
    task_with_memory *Task = 0;
    while (!Task)
    {
        Task = BeginTaskWithMemory(State);
    }
    
    receive_and_send_work *Work = PushStruct(&Task->Arena, receive_and_send_work);
    Work->IncomingAddress = *IncomingAddress; // deep copy so that other threads don't mutate what we use
    
    Work->ClientSocket = ClientSocket;
    Work->Task = Task;
    Work->State = State;
    
    State->PlatformAddEntry(Queue, ReceiveAndSend, Work);
}