#include <rtc/rtc.hpp>
#include <json.hpp>
#include <vector>
#include <fstream>
#include <compare>
#include <functional>

#include "accessor/export.hpp"

#define MAX_RTP_SIZE 10000

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#elif __linux__
#include <sys/socket.h>
#include <sys/types.h> 
#include <netinet/in.h>

void error(const char *msg)
{
    perror(msg);
    exit(1);
}
#endif

namespace AC
{

  struct ACCESSOR_EXPORT BridgeSocket
  {

    bool Valid = false;
    std::string Address;

    
#ifdef _WIN32
    int Port;

    SOCKET sock_;
    sockaddr_in addr_;
#elif __linux__
    int sockfd, newsockfd, portno;
    socklen_t clilen;
    char buffer[MAX_RTP_SIZE];
    struct sockaddr_in serv_addr, cli_addr;
    int n;
#endif

    static BridgeSocket GetFreeSocketPort(std::string adr = "127.0.0.1")
    {
      BridgeSocket s;
      s.Address = adr;
      s.Valid = false;
      
#ifdef _WIN32
      sockaddr info;
      int size = sizeof(addr_);
      s.sock_ = socket(AF_INET,SOCK_DGRAM,0);
      s.addr_.sin_addr.s_addr = inet_addr(adr.c_str());
      s.addr_.sin_port = htons(0);
      s.addr_.sin_family = AF_INET;
      getsockname(s.sock_,&info,&size);
      s.Port = *reinterpret_cast<int*>(info.sa_data);
      return s;
#elif __linux__
      s.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
      if (s.sockfd < 0)
      {
        return s; 
      }
      bzero((char*)&s.serv_addr, sizeof(serv_addr));
      s.portno = 0;

      s.serv_addr.sin_family = AF_INET;
      s.serv_addr.sin_addr.s_addr = INADDR_ANY;
      s.serv_addr.sin_port = htons(s.portno);
      if (bind(s.sockfd, (struct sockaddr*)&s.serv_addr,
        sizeof(s.serv_addr)) < 0)
        error("ERROR on binding");
#endif

      s.Valid = true;
      return s;
    }
  };

  struct ACCESSOR_EXPORT SaveRTP
  {
    uint32_t timestamp{0};
    uint32_t ssrc{0};
    uint16_t sequence{0};
    uint8_t payload_type{};
    bool has_padding{false};
    std::vector<std::byte> body;
    SaveRTP(){body.reserve(2048); }
    SaveRTP(rtc::RTP* package)
    {
      timestamp = package->timestamp();
      ssrc = package->ssrc();
      sequence = package->seqNumber();
      body.insert(
        body.end(),
        (std::byte*)package->getBody(),
        (std::byte*)package->getBody() + package->getSize()
      );
      payload_type = package->payloadType();
      has_padding = package->padding();
    }
    SaveRTP& operator=(rtc::RTP* package)
    {
      timestamp = package->timestamp();
      ssrc = package->ssrc();
      sequence = package->seqNumber();
      payload_type = package->payloadType();
      has_padding = package->padding();
      body.clear();
      body.insert(
        body.end(),
        (std::byte*)package->getBody(),
        (std::byte*)package->getBody() + package->getSize()
      );
      return *this;
    }

    inline void decodeH264Header()
    {
      
    }

    std::strong_ordering operator <=>(const auto& other) const {
      return (timestamp <=> other.timestamp == 0) ? sequence <=> other.sequence : timestamp<=> other.timestamp;
      //return sequence <=> other.sequence;
    }

  };

  class ACCESSOR_EXPORT Seeker: std::enable_shared_from_this<Seeker>
  {
  public:
    using json = nlohmann::json;
    Seeker();
    ~Seeker();
    virtual bool CheckSignallingActive();
    virtual void UseConfig(std::string filename);
    virtual bool EstablishedConnection(std::string ip = "127.0.0.1");
    virtual void FindBridge();
    virtual void RecoverConnection();

    

  protected:
    std::vector<BridgeSocket> Sockets;
    
  }; 
  
}
