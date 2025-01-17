

#pragma once
#ifndef WEBRTCBRIDGE_HPP
#define WEBRTCBRIDGE_HPP
#include <json.hpp>
#include <span>
#include <variant>
#include <chrono>
#include <fstream>
#include <queue>
#include <ostream>
#include <rtc/rtc.hpp>
#include "Synavis/export.hpp"

#define MAX_RTP_SIZE 208 * 1024

#define AS_UINT8(x) reinterpret_cast<uint8_t*>(x)
#define AS_CUINT8(x) reinterpret_cast<const uint8_t*>(x)
#define AS_BYTE(x) reinterpret_cast<std::byte*>(x)
#define AS_CBYTE(x) reinterpret_cast<const std::byte*>(x)

#if defined _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>

bool ParseTimeFromString(std::string Source, std::chrono::utc_time<std::chrono::system_clock::duration>& Destination);

#elif __linux__
#include <sys/socket.h>
#include <sys/types.h> 
#include <netinet/in.h>
#include <arpa/inet.h>
#include <date/date.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <fcntl.h>
bool ParseTimeFromString(std::string Source, std::chrono::time_point<std::chrono::system_clock>& Destination);

#endif

namespace Synavis
{

  template < typename T > std::weak_ptr<T> weaken(std::shared_ptr<T> const& ptr)
  {
    return std::weak_ptr<T>(ptr);
  }

  // a function to insert any variable into an rtc::binary object
  template < typename T > std::size_t InsertIntoBinary(rtc::binary& Binary, std::size_t offset, T Data)
  {
    const std::size_t  size = Binary.size();
    memcpy(Binary.data() + offset, &Data, sizeof(Data));
    return size;
  }
  template < typename T, typename... Args > std::size_t InsertIntoBinary(rtc::binary& Binary, std::size_t offset, T Data, Args... args)
  {
    const std::size_t  size = Binary.size();
    memcpy(Binary.data() + offset, &Data, sizeof(Data));
    return InsertIntoBinary(Binary, offset + sizeof(Data), args...);
  }

  // a type trait for checking whether a container can be converted to a pointer
  // this is to avoid the unfortunate type confusion in MSVC about rtc::binary
  // even if it is a std::vector<uint8_t> but it won't convert the types to one another
  template < typename T, typename _ = void >
  struct is_pointer_convertible : std::false_type
  {
  };
  template < typename ... Runoff >
  struct is_pointer_convertible_helper {};

  template < typename T >
  struct is_pointer_convertible<T,
    std::conditional_t<
    false,
    is_pointer_convertible_helper<
    typename T::value_type,
    typename T::size_type,
    decltype(std::declval<T>().data()),
    decltype(std::declval<T>().size())
    >, void >> : public std::true_type
  {
  };

  // inline function to retrieve the byte size of a buffer
  template < typename T >
  static size_t ByteSize(const T& Data)
  {
    // check if the data is convertible to a pointer
    static_assert(is_pointer_convertible<T>::value, "Data must be convertible to a pointer");
    return Data.size() * sizeof(decltype(*Data.data()));
  }

  // a function to encode a rtc::binary object into a base64 string
  // adapted from https://stackoverflow.com/questions/180947/base64-decode-snippet-in-c
  template < typename T >
  static std::string_view Encode64(const T& Data)
  {
    // check if the data is convertible to a pointer
    static_assert(is_pointer_convertible<T>::value, "Data must be convertible to a pointer");
    std::size_t byte_size = Data.size() * sizeof(decltype(*Data.data()));
    // convert data to string of bytes
    std::string_view strdata(reinterpret_cast<const char*>(Data.data()), byte_size);
    // base64 encoding table
    constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    // compute the size of the encoded string
    std::size_t encoded_length = 4 * ((byte_size + 2) / 3);
    char* c = new char[encoded_length];
    std::string_view c_result = { c, encoded_length };
    std::size_t i;
    for (i = 0; i < byte_size - 2; i += 3)
    {
      *c++ = table[(strdata[i] >> 2) & 0x3F];
      *c++ = table[((strdata[i] & 0x3) << 4) | ((int)(strdata[i + 1] & 0xF0) >> 4)];
      *c++ = table[((strdata[i + 1] & 0xF) << 2) | ((int)(strdata[i + 2] & 0xC0) >> 6)];
      *c++ = table[strdata[i + 2] & 0x3F];
    }
    if (i < byte_size)
    {
      *c++ = table[(strdata[i] >> 2) & 0x3F];
      if (i == (byte_size - 1))
      {
        *c++ = table[((strdata[i] & 0x3) << 4)];
        *c++ = '=';
      }
      else
      {
        *c++ = table[((strdata[i] & 0x3) << 4) | ((int)(strdata[i + 1] & 0xF0) >> 4)];
        *c++ = table[((strdata[i + 1] & 0xF) << 2)];
      }
      *c++ = '=';
    }
    // return the encoded string as a string_view because this way the data
    // is not deleted when the scope ends in which this function is called
    return  c_result;
  }

  // a function to retrieve the encoded size of a buffer for base64 encoding
  template < typename T >
  static size_t EncodedSize(const T& Data)
  {
    // check if the data is convertible to a pointer
    static_assert(is_pointer_convertible<T>::value, "Data must be convertible to a pointer");
    // compute the size of the encoded string
    std::size_t encoded_length = 4 * ((((Data.size() * sizeof(decltype(*Data.data())))) + 2) / 3);
    return encoded_length;
  }

  void ExitWithMessage(std::string Message, int Code = 0);

  // forward definitions
  class Adapter;

  int64_t TimeSince(std::chrono::system_clock::time_point t);
  double HighRes();

  std::string GetLocalIP();
  std::string FormattedTime(std::chrono::system_clock::time_point Time, bool ms = false);

  inline void SYNAVIS_EXPORT VerboseMode()
  {
    rtcInitLogger(RTC_LOG_VERBOSE, nullptr);
  }

  inline void SYNAVIS_EXPORT SilentMode()
  {
    rtcInitLogger(RTC_LOG_NONE, nullptr);
  }

  inline std::ofstream SYNAVIS_EXPORT OpenUniqueFile(std::string Base)
  {
    // remove ending
    std::string Filename = Base;
    // remove file extension
    Filename.erase(Filename.find_last_of("."), std::string::npos);
    // add timestamp
    Filename += "_" + FormattedTime(std::chrono::system_clock::now());
    // add file extension
    Filename += ".log";
    return std::ofstream(Filename, std::ios::app);
  }

  // a class to represent access to a buffer in reverse byte order
  template < typename T >
  class SYNAVIS_EXPORT EndianBuffer
  {
    std::span<uint8_t> Data;
    EndianBuffer(const std::vector<T>& Data)
    {
      Data = std::span<uint8_t>(reinterpret_cast<uint8_t*>(Data.data()), Data.size() * sizeof(T));
    }
    EndianBuffer(const std::span<T>& Data)
    {
      Data = std::span<uint8_t>(reinterpret_cast<uint8_t*>(Data.data()), Data.size() * sizeof(T));
    }
    uint8_t& operator[](std::size_t index)
    {
      // reverse packs of bytes of size T in the buffer
      const std::size_t size = sizeof(T);
      const std::size_t offset = (index / size) * size; // integer division
      const std::size_t remainder = index % size;
      return Data[offset + (size - remainder - 1)];
    }
    const T at(std::size_t index) const
    {
      // returns the object at the index in the original type, with the bytes reversed
      const std::size_t size = sizeof(T);
      const std::size_t offset = (index / size) * size; // integer division
      const std::size_t remainder = index % size;
      union
      {
        T value;
        uint8_t bytes[size];
      } result;
      for (std::size_t i = 0; i < size; ++i)
      {
        result.bytes[i] = Data[offset + (size - i - 1)];
      }
      return result.value;
    }
  };

  class SYNAVIS_EXPORT BridgeSocket
  {
  public:
    bool Valid = false;
    bool Outgoing = false;
    std::string Address;
    void SetAddress(std::string inAddress);
    std::string GetAddress();
    void SetBlockingEnabled(bool Blocking = true);
    char* Reception;
    std::span<std::byte> BinaryData;
    std::span<std::size_t> NumberData;
    std::string_view StringData;

    std::size_t ReceivedLength;
    BridgeSocket();
    BridgeSocket(BridgeSocket&& other) = default;
    ~BridgeSocket();
    int Port;
    int GetSocketPort();
    void SetSocketPort(int Port);

    std::string What();

#ifdef _WIN32
    SOCKET Sock{ INVALID_SOCKET };
    sockaddr info;
    struct sockaddr_in Addr, Remote;
#elif __linux__
    int Sock, newsockfd;
    socklen_t clilen;
    struct sockaddr_in Addr;
    struct sockaddr_in Remote;
    int n;
#endif

    // this method connects the Socket to its respective output or input
    // in the input case, the address should be set to the remote end
    // it will automatically call either bind or connect
    // remember that this class is connectionless
    bool Connect();
    void Disconnect();

    int ReadSocketFromBinding();

    static BridgeSocket GetFreeSocket(std::string adr = "127.0.0.1");

    int Peek();
    virtual int Receive(bool invalidIsFailure = false);
    virtual bool Send(std::variant<rtc::binary, std::string> message);

    template < typename N >
    std::span<N> Reinterpret()
    {
      return std::span<N>(reinterpret_cast<N*>(Reception), ReceivedLength / sizeof(N));
    }
  };

#pragma pack(push, 1)
  struct SYNAVIS_EXPORT BridgeRTPHeader
  {
    // Extension Header
    uint16_t profile_id{ 1667 };
    uint16_t length{ sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t) };

    // Actual extension values
    uint16_t player_id;
    uint16_t streamer_id;
    uint32_t meta;
  };
#pragma pack(pop)

  enum class SYNAVIS_EXPORT EClientMessageType
  {
    QualityControlOwnership = 0u,
    Response,
    Command,
    FreezeFrame,
    UnfreezeFrame,
    VideoEncoderAvgQP,
    LatencyTest,
    InitialSettings,
    TestEcho
  };

  enum class SYNAVIS_EXPORT EConnectionState
  {
    STARTUP = (std::uint8_t)EClientMessageType::InitialSettings + 1u,
    SIGNUP,
    OFFERED,
    CONNECTED,
    VIDEO,
    CLOSED,
    RTCERROR,
  };

  enum class SYNAVIS_EXPORT EBridgeConnectionType
  {
    LockedMode = (std::uint8_t)EConnectionState::RTCERROR + 1u,
    BridgeMode,
    DirectMode
  };

  enum class SYNAVIS_EXPORT EDataReceptionPolicy
  {
    TempFile = (std::uint8_t)EBridgeConnectionType::DirectMode + 1u,
    BinaryCallback,
    SynchronizedMetadata,
    AsynchronousMetadata,
    JsonCallback,
    Loss
  };

  enum class SYNAVIS_EXPORT EMessageTimeoutPolicy
  {
    Silent = (std::uint8_t)EDataReceptionPolicy::Loss + 1u,
    Critical,
    All
  };

  enum class SYNAVIS_EXPORT ELogVerbosity
  {
    Silent = (std::uint8_t)EMessageTimeoutPolicy::All + 1u,
    Error,
    Warning,
    Info,
    Debug,
    Verbose
  };

  enum class SYNAVIS_EXPORT ECodec
  {
    VP8 = (std::uint8_t)ELogVerbosity::Verbose + 1u,
    VP9,
    H264,
    H265,
    None
  };

  // a simple logger for the library
  class SYNAVIS_EXPORT Logger
  {
  public:
    class LoggerInstance
    {
      LoggerInstance(const LoggerInstance&) = delete;
      LoggerInstance& operator=(const LoggerInstance&) = delete;
      LoggerInstance(LoggerInstance&&) = delete;
      LoggerInstance& operator=(LoggerInstance&&) = delete;
    private:
      friend class Logger;
      LoggerInstance(std::string Instigator)
      {
        this->Instigator = Instigator;
        this->Parent = Logger::Get();
      }
      inline std::string TimeStamp() const
      {
        return FormattedTime(std::chrono::system_clock::now(), true);
      }

      std::string Instigator;
      Logger* Parent;
    public:
      ~LoggerInstance() = default;
      template < typename T >
      Logger& operator<<(T&& Message) const
      {
        *Parent << "[" << Instigator << "]"
          << "[" << TimeStamp() << "]: "
          << Message;
        return *Parent;
      }
      const LoggerInstance& operator()(ELogVerbosity V) const
      {
        Parent->SetState(V);
        return *this;
      }

    };
    // singleton
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;
    ~Logger()
    {
      if (LogFile)
      {
        LogFile->flush();
        delete LogFile;
      }
    }

    static Logger* Get()
    {
      static Logger instance;
      return &instance;
    }

    LoggerInstance LogStarter(std::string Instigator) const
    {
      return LoggerInstance(Instigator);
    }

    void SetupLogfile(std::string Filename)
    {
      LogFile = new std::ofstream(OpenUniqueFile(Filename));
      // combine file and cout streams
      LogFile->rdbuf()->pubsetbuf(0, 0);
    }

    void SetupLogfile(std::ofstream&& File)
    {
      LogFile = new std::ofstream(std::move(File));
      // combine file and cout streams
      LogFile->rdbuf()->pubsetbuf(0, 0);
    }

    // two stream operators for logging
    // first use of << will be the verbosity
    // second use of << will be the message
    template < typename T >
    Logger& operator<<(T&& Message)
    {
      if (this->StatusVerbosity.load() <= Verbosity)
      {
        if (LogFile)
        {
          *LogFile << Message;
        }
        if (this->StatusVerbosity <= ELogVerbosity::Error)
          std::cerr << Message;
        else
          std::cout << Message;
      }
      return *this;
    }

    void SetState(ELogVerbosity V) const
    {
      this->StatusVerbosity.store(V);
    }

    void SetVerbosity(ELogVerbosity V)
    {
      this->Verbosity = V;
    }

    ELogVerbosity GetVerbosity() const
    {
      return Verbosity;
    }

    void SetVerbosity(std::string V)
    {
      // make V lowercase
      std::transform(V.begin(), V.end(), V.begin(), ::tolower);
      if (V == "silent")
      {
        this->Verbosity = ELogVerbosity::Silent;
      }
      else if (V == "error")
      {
        this->Verbosity = ELogVerbosity::Error;
      }
      else if (V == "warning")
      {
        this->Verbosity = ELogVerbosity::Warning;
      }
      else if (V == "info")
      {
        this->Verbosity = ELogVerbosity::Info;
      }
      else if (V == "debug")
      {
        this->Verbosity = ELogVerbosity::Debug;
      }
      else if (V == "verbose")
      {
        this->Verbosity = ELogVerbosity::Verbose;
      }
    }

    // reset state when std::endl or std::flush is detected
    Logger& operator<<(std::ostream& (*pf)(std::ostream&))
    {
      if (this->StatusVerbosity.load() <= ELogVerbosity::Error)
      {
        std::cerr << std::endl;
        if (LogFile)
        {
          *LogFile << std::endl;
        }
      }
      else if (this->StatusVerbosity.load() <= Verbosity)
      {
        if (LogFile)
        {
          *LogFile << std::endl;
        }
        std::cout << std::endl;
      }
      this->StatusVerbosity.store(ELogVerbosity::Silent);
      return *this;
    }

  private:
    Logger() = default;

    std::ostream* LogFile = nullptr;
    ELogVerbosity Verbosity = ELogVerbosity::Info;
    mutable std::atomic<ELogVerbosity> StatusVerbosity;
  };

  using StreamVariant = std::variant<std::shared_ptr<rtc::DataChannel>,
    std::shared_ptr<rtc::Track>>;

  class SYNAVIS_EXPORT CommandLineParser
  {
  public:
    CommandLineParser(int argc, char** argv);
    std::string GetArgument(std::string Name);
    bool HasArgument(std::string Name);
  private:
    std::unordered_map<std::string, std::string> Arguments;
  };

  class SYNAVIS_EXPORT NoBufferThread
  {
  public:
    const int ReceptionSize = 208 * 1024 * 1024;
    uint32_t RtpDestinationHeader{};
    EBridgeConnectionType ConnectionMode{ EBridgeConnectionType::DirectMode };
    NoBufferThread(std::shared_ptr<BridgeSocket> inSocketConnection);
    std::size_t AddRTC(StreamVariant inRTC);
    std::size_t AddRTC(StreamVariant&& inRTC);
    void Run();
  private:
    std::future<void> Thread;
    std::map<std::size_t, StreamVariant> WebRTCTracks;
    std::shared_ptr<BridgeSocket> SocketConnection;
  };

  class SYNAVIS_EXPORT WorkerThread
  {
  public:
    WorkerThread();
    ~WorkerThread();
    void Run();
    void AddTask(std::function<void(void)>&& Task);
    void Stop();
    uint64_t GetTaskCount();
  private:
    std::future<void> Thread;
    std::mutex TaskMutex;
    std::queue<std::function<void(void)>> Tasks;
    std::condition_variable TaskCondition;
    bool Running = true;
  };

  class SYNAVIS_EXPORT Bridge
  {
  public:
    using json = nlohmann::json;
    Bridge();
    virtual ~Bridge();
    virtual std::string Prefix();
    void SetTimeoutPolicy(EMessageTimeoutPolicy inPolicy, std::chrono::system_clock::duration inTimeout);
    EMessageTimeoutPolicy GetTimeoutPolicy();
    void UseConfig(std::string filename);
    void UseConfig(json Config);
    virtual void BridgeSynchronize(Adapter* Instigator,
      json Message, bool bFailIfNotResolved = false);
    void CreateTask(std::function<void(void)>&& Task);
    void BridgeSubmit(Adapter* Instigator, StreamVariant origin, std::variant<rtc::binary, std::string> Message) const;
    virtual void InitConnection();
    void SetHeaderByteStart(uint32_t Byte);
    virtual void BridgeRun();
    virtual void Listen();
    virtual bool CheckSignallingActive();
    virtual bool EstablishedConnection(bool Shallow = true);
    virtual void FindBridge();
    virtual void StartSignalling(std::string IP, int Port, bool keepAlive = true, bool useAuthentification = false);
    void ConfigureTrackOutput(std::shared_ptr<rtc::Track> OutputStream, rtc::Description::Media* Media);
    void SubmitToSignalling(json Message, Adapter* Endpoint);
    inline bool FindID(const json& Jason, int& ID)
    {
      for (auto it = Jason.begin(); it != Jason.end(); ++it)
      {
        for (auto name : { "id", "player_id", "app_id" })
        {
          if (it.key() == name && it.value().is_number_integer())
          {
            ID = it.value().get<int>();
            return true;
          }
        }
      }
      return false;
    }
    // This method should be used to signal to the provider
    // that a new application has connected.
    virtual uint32_t SignalNewEndpoint() = 0;
    virtual void OnSignallingMessage(std::string Message) = 0;
    virtual void RemoteMessage(json Message) = 0;
    virtual void OnSignallingData(rtc::binary Message) = 0;
    void Stop();
  protected:
    EMessageTimeoutPolicy TimeoutPolicy;
    std::chrono::system_clock::duration Timeout;
    json Config{
    {
      {"LocalPort", int()},
      {"RemotePort",int()},
      {"LocalAddress",int()},
      {"RemoteAddress",int()},
      {"Signalling",int()}
    } };
    std::unordered_map<int, std::shared_ptr<Adapter>> EndpointById;
    std::future<void> BridgeThread;
    std::mutex QueueAccess;
    std::queue<std::function<void(void)>> CommInstructQueue;
    std::future<void> ListenerThread;
    std::mutex CommandAccess;
    std::queue<std::variant<rtc::binary, std::string>> CommandBuffer;
    std::condition_variable CommandAvailable;
    bool bNeedInfo{ false };
    // Signalling Server
    std::shared_ptr<rtc::WebSocket> SignallingConnection;
    EBridgeConnectionType ConnectionMode{ EBridgeConnectionType::BridgeMode };
    std::shared_ptr<NoBufferThread> DataInThread;
    struct
    {
      std::shared_ptr<BridgeSocket> In;
      std::shared_ptr<BridgeSocket> Out;
      // Data out is being called without locking!
      // There should be no order logic behind the packages, they should just be sent as-is!
      std::shared_ptr<BridgeSocket> DataOut;
    } BridgeConnection;
    std::condition_variable TaskAvaliable;
    // this will be set the first time an SDP is transmitted
    // this will be asymmetric because UE has authority
    // over the header layout
    uint32_t RtpDestinationHeader{};

    int NextID{ 0 };
  private:
    bool Run = true;
  };
}
#endif
