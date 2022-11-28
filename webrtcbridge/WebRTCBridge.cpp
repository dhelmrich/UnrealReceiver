#include "WebRTCBridge.hpp"
#include "Adapter.hpp"

#include <variant>
#include <fstream>
#include <span>

int WebRTCBridge::BridgeSocket::Receive(bool invalidIsFailure)
{
#ifdef _WIN32
  int length = sizeof(Remote);
  auto size = recvfrom(Sock,Reception,MAX_RTP_SIZE,0,reinterpret_cast<sockaddr*>(&Remote),&length);
  //auto size = recv(Sock,Reception,MAX_RTP_SIZE,0);
  if(size < 0)
  {
    std::cout << "Encountered error!" << std::endl;
    return 0;
  }
  StringData = std::string_view(Reception,size);
  return size;
#elif __linux__
#include <sys/socket.h>
  socklen_t length = static_cast<socklen_t>(sizeof(Remote));
  auto size = recvfrom(Sock, Reception, MAX_RTP_SIZE, 0, reinterpret_cast<struct sockaddr*>(&Remote), &length);
  if(size < 0)
  {
    std::cout << "Encountered error!" << std::endl;
    return 0;
  }
  StringData = std::string_view(Reception,size);
  return 0;
#endif
}

void WebRTCBridge::BridgeSocket::SetAddress(std::string inAddress)
{ this->Address = inAddress; }

std::string WebRTCBridge::BridgeSocket::GetAddress()
{ return this->Address; }

void WebRTCBridge::BridgeSocket::SetBlockingEnabled(bool Blocking)
{
  unsigned long Mode = !Blocking;
#ifdef _WIN32
  ioctlsocket(Sock, FIONBIO, &Mode);
#endif
}

WebRTCBridge::BridgeSocket::BridgeSocket():Reception(new char[MAX_RTP_SIZE])
{
}

WebRTCBridge::BridgeSocket::~BridgeSocket()
{
    
#ifdef _WIN32
  auto result = shutdown(Sock,SD_BOTH);
  if (result == SOCKET_ERROR) {
    closesocket(Sock);
    WSACleanup();
  }
#elif defined __linux__
      shutdown(Sock,2);
#endif
  delete[] Reception;
}

int WebRTCBridge::BridgeSocket::GetSocketPort()
{return Port;}

void WebRTCBridge::BridgeSocket::SetSocketPort(int Port)
{this->Port = Port;}

bool WebRTCBridge::BridgeSocket::Connect()
{
  if(Address == "localhost")
  {
    Address = "127.0.0.1";
  }
#ifdef _WIN32
  int size = sizeof(Addr);
  WSADATA wsaData;
  auto iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
  if ( iResult != 0 )
  {
    return false;
  }
  // we are employing connectionless sockets for the transmission
  // as we are using a constant udp stream that we are forwarding
  // The important note here is that the stream is connectionless
  // and we are doing this because we do not want any recv/rep pattern
  // breaking the logical flow of the program
  // the bridge itself is also never waiting for answers and as such,
  // will use a lazy-style send/receive pattern that will will be able
  // to parse answers without needing a strict order of things to do
  // ...
  // this is also why there are so many threads in this program.
  // ...
  // That being said, I am also not super keen on this setup, so any
  // suggestion is always welcome.IPPROTO_HOPOPTS
  Sock = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
  Addr.sin_addr.s_addr=inet_addr(Address.c_str());
  Addr.sin_port = htons(Port);
  Addr.sin_family = AF_INET;
  //getsockname(NULL,reinterpret_cast<sockaddr*>(&Addr),&size);
  //Port = *reinterpret_cast<int*>(info.sa_data);
  if(Outgoing)
  {
    if(connect(Sock,reinterpret_cast<sockaddr*>(&Addr),sizeof(sockaddr_in)) == SOCKET_ERROR)
    {
      closesocket(Sock);
      WSACleanup();
      return false;
    }
    else
    {
      return Sock != INVALID_SOCKET;
    }
  }
  else
  {
    if(bind(Sock,reinterpret_cast<sockaddr*>(&Addr),sizeof(sockaddr_in)) == SOCKET_ERROR)
    {
      closesocket(Sock);
      WSACleanup();
      return false;
    }
    else
    {
      return Sock != INVALID_SOCKET;
    }
  }
#elif defined __linux__
  Sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (Sock < 0)
  {
    return false; 
  }
  bzero((char*)&Addr, sizeof(Addr));
  Addr.sin_family = AF_INET;
  Addr.sin_addr.s_addr = INADDR_ANY;
  Addr.sin_port = htons(Port);
  if (bind(Sock, (struct sockaddr*)&Addr,
    sizeof(Addr)) < 0)
  {
    return false;
  }
  return true;
#endif
    
}

int WebRTCBridge::BridgeSocket::ReadSocketFromBinding()
{
#ifdef _WIN32
  int size = sizeof(info);
  getsockname(Sock,&info,&size);
  Port = *reinterpret_cast<int*>(info.sa_data);
  return Port;
#elif defined __linux__
#endif
  return 0;
}

WebRTCBridge::BridgeSocket WebRTCBridge::BridgeSocket::GetFreeSocket(std::string adr)
{
  BridgeSocket s;
  s.Address = adr;
  s.Valid = false;
      
#ifdef _WIN32
  int size = sizeof(Addr);
  s.Sock = socket(AF_INET,SOCK_DGRAM,0);
  s.Addr.sin_addr.s_addr = inet_addr(adr.c_str());
  s.Addr.sin_port = htons(s.Port);
  s.Addr.sin_family = AF_INET;
  getsockname(s.Sock,&s.info,&size);
  s.Port = *reinterpret_cast<int*>(s.info.sa_data);
  return s;
#elif __linux__
  s.Sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (s.Sock < 0)
  {
    return s; 
  }
  bzero((char*)&s.Addr, sizeof(Addr));
  s.Port = 0;

  s.Addr.sin_family = AF_INET;
  s.Addr.sin_addr.s_addr = INADDR_ANY;
  s.Addr.sin_port = htons(s.Port);
  if (bind(s.Sock, (struct sockaddr*)&s.Addr,
    sizeof(s.Addr)) < 0)
    throw std::runtime_error("Could not connect to socket.");
#endif

  s.Valid = true;
  return s;
}

int WebRTCBridge::BridgeSocket::Peek()
{
  SetBlockingEnabled(false);
#ifdef _WIN32
  auto size = recv(Sock,Reception,MAX_RTP_SIZE,MSG_PEEK);
  if(size < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
  {
    return 0;
  }
  else
  {
    StringData = std::string_view(Reception,size);
  }
  SetBlockingEnabled(true);
#elif defined __linux__
  int size = 0;
#endif
  return size;
}

void WebRTCBridge::BridgeSocket::Send(std::variant<rtc::binary, std::string> message)
{
  if(Outgoing)
  {
    const char* buffer;
    int length;
    if(std::holds_alternative<std::string>(message))
    {
      buffer = std::get<std::string>(message).c_str();
      length = static_cast<int>(std::get<std::string>(message).length());
    }
    else if (std::holds_alternative<rtc::binary>(message))
    {
      buffer = reinterpret_cast<const char*>(std::get<rtc::binary>(message).data());
      length = static_cast<int>(std::get<rtc::binary>(message).size());
    }
    int status;
    if((status = send(Sock,buffer,length,0)) == 0)
    //if((status = sendto(Sock, buffer, length, 0, reinterpret_cast<sockaddr*>(&Addr), sizeof(sockaddr_in))) == 0)
    {
      std::cout << "What:" << status << std::endl;
      exit(-1);
    }
    else
    {
    }
  }
}

WebRTCBridge::NoBufferThread::NoBufferThread(std::shared_ptr<BridgeSocket> inDataSource)
    : SocketConnection(inDataSource)
{
  Thread = std::async(&WebRTCBridge::NoBufferThread::Run,this);
}

std::size_t WebRTCBridge::NoBufferThread::AddRTC(StreamVariant inRTC)
{

  return std::size_t();
}

std::size_t WebRTCBridge::NoBufferThread::AddRTC(StreamVariant&& inRTC)
{

  return std::size_t();
}

void WebRTCBridge::NoBufferThread::Run()
{
  // Consume buffer until close (this should never be empty but we never know)
  
  int Length;
  while ((Length = SocketConnection->Receive()) > 0)
  {
    if (Length < sizeof(rtc::RtpHeader))
      continue;
    if (ConnectionMode == EBridgeConnectionType::LockedMode)
    {
      // we gather all packages and submit them together, we will also discard packages
      // that are out of order
    }
    else
    {
      // TOOD if this check fails we might run into \0 at the end of the string.

      const auto& destination = WebRTCTracks[SocketConnection->NumberData[0]];
      const auto& byte_data = SocketConnection->BinaryData;

      rtc::RtpHeader* head = reinterpret_cast<rtc::RtpHeader*>(byte_data.data());
      auto target = head->getExtensionHeader() + this->RtpDestinationHeader;


      if(std::holds_alternative<std::shared_ptr<rtc::Track>>(destination))
      {
        std::get<std::shared_ptr<rtc::Track>>(destination)->send(byte_data.data(), byte_data.size());
      }
      else
      {
        std::get< std::shared_ptr<rtc::DataChannel> >(destination)->send(byte_data.data(), byte_data.size());
      }
    }
    // This is a roundabout reinterpret_cast without having to actually do one
    //DataDestinationPtr->Send(SocketConnection->BinaryData.data(),Length);
  }
}

WebRTCBridge::Bridge::Bridge()
{
  std::cout << "An instance of the WebRTCBridge was started, we are starting the threads..." << std::endl;
  BridgeThread = std::async(std::launch::async, &Bridge::BridgeRun,this);
  std::cout << "Bridge Thread started" << std::endl;
  ListenerThread = std::async(std::launch::async,&Bridge::Listen, this);
  std::cout << "Listener Thread Started" << std::endl;

  BridgeConnection.In = std::make_shared<BridgeSocket>();
  BridgeConnection.Out = std::make_shared<BridgeSocket>();
  BridgeConnection.DataOut = std::make_shared<BridgeSocket>();
}

WebRTCBridge::Bridge::~Bridge()
{
}

void WebRTCBridge::Bridge::BridgeSynchronize(Adapter* Instigator, nlohmann::json Message, bool bFailIfNotResolved)
{
  if(Instigator != nullptr)
    Message["id"] = Instigator->ID;
  else
    Message["id"] = -1;
  std::string Transmission = Message.dump();
  BridgeConnection.Out->Send(Transmission);
  auto messagelength = BridgeConnection.In->Receive(true);
  if (messagelength <= 0)
  {
    if (bFailIfNotResolved)
    {
      throw std::domain_error(std::string("Could not receive answer from Bridgehead and this synchronization is critical:\n\n")
        + "Message was:\n\n"
        + Message.dump(1, '\t'));
    }
  }
  else
  {
    json Answer;
    try
    {
      Answer = json::parse(BridgeConnection.In->StringData);
    }
    catch (std::exception e)
    {
      if (bFailIfNotResolved)
      {
        throw std::runtime_error(std::string("An error occured while parsing the Bridge response:\n\n")
          + e.what());
      }
    }
    catch (...)
    {
      if (bFailIfNotResolved)
      {
        throw std::runtime_error("An unexpected error occured while parsing the Bridge response");
      }
    }
    if(Answer["type"] == "ok")
    {
      
    }
    else if(Answer["type"] == "todo")
    {
      Instigator->OnRemoteInformation(Answer);
    }
  }
}

void WebRTCBridge::Bridge::CreateTask(std::function<void()>&& Task)
{
  std::unique_lock<std::mutex> lock(QueueAccess);
  lock.lock();
  CommInstructQueue.push(Task);
  lock.unlock();
}

void WebRTCBridge::Bridge::BridgeSubmit(Adapter* Instigator, StreamVariant origin, std::variant<rtc::binary, std::string> Message) const
{
  // we need to break this up because of json lib compatibility
  if (std::holds_alternative<std::string>(Message))
  {
    json Transmission = { {"id",Instigator->ID} };
    Transmission["data"] = std::get<std::string>(Message);
    BridgeConnection.Out->Send(Transmission);
  }
  else
  {
    auto data = std::get<rtc::binary>(Message);
    if(data.size() > RtpDestinationHeader + 13)
    {
      auto* p_proxy = reinterpret_cast<uint16_t*>((data.data() + RtpDestinationHeader));
      *p_proxy = Instigator->ID;
    }
    BridgeConnection.DataOut->Send(data);
  }
}

void WebRTCBridge::Bridge::InitConnection()
{
  BridgeConnection.In->Address = Config["RemoteAddress"];
  BridgeConnection.In->Port = Config["RemotePort"];
  if(!BridgeConnection.In->Connect())
  {
    throw std::runtime_error("Unexpected error when connecting to an incoming socket:");
  }
}

void WebRTCBridge::Bridge::SetHeaderByteStart(uint32_t Byte)
{
  this->DataInThread->RtpDestinationHeader = Byte;
}

void WebRTCBridge::Bridge::BridgeRun()
{
  std::unique_lock<std::mutex> lock(QueueAccess);
  while (true)
  {
    TaskAvaliable.wait(lock, [this] {
      return (CommInstructQueue.size());
      });
    if (CommInstructQueue.size() > 0)
    {
      auto Task = std::move(CommInstructQueue.front());
      lock.unlock();
      Task();

      // locking at the end of the loop is necessary because next
      // top start of this scope requiers there to be a locked lock.
      lock.lock();
    }
  }
}

void WebRTCBridge::Bridge::Listen()
{
  std::unique_lock<std::mutex> lock(CommandAccess);
  while (true)
  {
    CommandAvailable.wait(lock, [this]
      {
        return bNeedInfo && this->BridgeConnection.In->Peek() > 0;
      });
    bool isMessage = false;
    try
    {
      // all of these things must be available and also present
      // on the same layer of the json signal
      auto message = json::parse(this->BridgeConnection.In->StringData);
      std::string type = message["type"];
      auto app_id = message["id"].get<int>();
      
      EndpointById[app_id]->OnRemoteInformation(message);
    }
    catch (...)
    {

    }
  }
}

bool WebRTCBridge::Bridge::CheckSignallingActive()
{
  return SignallingConnection->isOpen();
}

bool WebRTCBridge::Bridge::EstablishedConnection(bool Shallow)
{
  using namespace std::chrono_literals;
  auto status_bridge_thread = BridgeThread.wait_for(0ms);
  auto status_command_thread = ListenerThread.wait_for(0ms);
  if(!Shallow)
  {
    // check connection validity here and actually ping connected bridges
  }
  return (BridgeConnection.In->Valid && BridgeConnection.Out->Valid
         && status_bridge_thread != std::future_status::ready
         && status_command_thread != std::future_status::ready);
}

void WebRTCBridge::Bridge::FindBridge()
{
  
}

void WebRTCBridge::Bridge::StartSignalling(std::string IP, int Port, bool keepAlive, bool useAuthentification)
{
  SignallingConnection = std::make_shared<rtc::WebSocket>();
  std::promise<void> RunGuard;
  auto Notifier = RunGuard.get_future();
  SignallingConnection->onOpen([this, &RunGuard]()
  {
    RunGuard.set_value();
  });
  SignallingConnection->onClosed([this, &RunGuard](){});
  SignallingConnection->onError([this, &RunGuard](auto error){});
  SignallingConnection->onMessage([this](auto message)
  {
    if(std::holds_alternative<std::string>(message))
    {
      OnSignallingMessage(std::get<std::string>(message));
    }
    else
    {
      OnSignallingData(std::get<rtc::binary>(message));
    }
  });
  if(useAuthentification)
  {
    // this is its own issue as we are probably obliged to conform to IDM guidelines here
    // these should all interface the same way, but here we should probably
    // call upon the jupyter-jsc service that should run in the background somewhere
  }
  else
  {
    SignallingConnection->open(IP + std::to_string(Port));
  }
  std::cout << "Waiting for Signalling Websocket to Connect." << std::endl;
  Notifier.wait();
}

void WebRTCBridge::Bridge::ConfigureTrackOutput(std::shared_ptr<rtc::Track> OutputStream, rtc::Description::Media* Media)
{
  // this is from media to track, as there is no direct function of configuring the track using the media
  // and the specific implementation of the rtc is largely hidden behind templated constructs.

  // there is nothing to be done here most likely, since the reporter hinge on the RTPPacketization
  // which we cannot provide as we do not actually packetize any frames
}

void WebRTCBridge::Bridge::SubmitToSignalling(json Message, Adapter* Endpoint)
{
  if(SignallingConnection->isOpen())
  {
    Message["ID"] = Endpoint->ID;
    SignallingConnection->send(Message);
  }
}

void WebRTCBridge::Bridge::UseConfig(std::string filename)
{
  std::ifstream file(filename);
  auto fileConfig = json::parse(file);
  UseConfig(fileConfig);
}

void WebRTCBridge::Bridge::UseConfig(json fileConfig)
{
  bool complete = true;
  for(auto key : Config)
    if(fileConfig.find(key) == fileConfig.end())
      complete = false;
  if(complete)
  {
    Config = fileConfig;
  }
}

#ifdef _WIN32
bool ParseTimeFromString(std::string Source, std::chrono::utc_time<std::chrono::system_clock::duration>& Destination)
{
  std::string format = "%Y-%m-%d %X";
  std::istringstream ss(Source);
  if(ss >> std::chrono::parse(format,Destination))
    return true;
  return false;
}
#elif defined __linux__
bool ParseTimeFromString(std::string Source, std::chrono::time_point<std::chrono::system_clock>& Destination)
{
  std::string format = "%Y-%m-%d %X";
  std::istringstream ss(Source);
  if(ss >> date::parse(format,Destination))
    return true;
  else return false;
}
#endif