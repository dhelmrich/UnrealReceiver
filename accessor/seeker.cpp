#include "seeker.hpp"

#include <variant>

#include "connector.hpp"

int AC::BridgeSocket::Peek()
{
return -1;
}

std::string AC::BridgeSocket::Copy()
{
  return std::string(Reception,ReceivedLength);
}

void AC::BridgeSocket::Send(std::variant<std::byte, std::string> message)
{
}

AC::Seeker::Seeker()
{
  BridgeThread = std::make_unique<std::thread>(&Seeker::BridgeRun,this);
  ListenerThread = std::make_unique<std::thread>(&Seeker::Listen, this);
}

AC::Seeker::~Seeker()
{
  
}

bool AC::Seeker::CheckSignallingActive()
{
  return false;
}

void AC::Seeker::UseConfig(std::string filename)
{
  std::ifstream file(filename);
  auto fileConfig = json::parse(file);
  bool complete = true;
  for(auto key : Config)
    if(fileConfig.find(key) == fileConfig.end())
      complete = false;
}

bool AC::Seeker::EstablishedConnection()
{
  BridgeConnection.In = std::make_shared<BridgeSocket>();
  BridgeConnection.Out = std::make_shared<BridgeSocket>();
  BridgeConnection.Out->Address = Config["LocalAddress"];
  BridgeConnection.Out->Port = Config["LocalPort"];
  BridgeConnection.In->Address = Config["RemoteAddress"];
  BridgeConnection.In->Port = Config["RemotePort"];
  if(BridgeConnection.Out->Connect() && BridgeConnection.In->Connect())
  {
    std::unique_lock<std::mutex> lock(QueueAccess);
    lock.lock();
    int PingPongSuccessful = -1;
    CommInstructQueue.push([this,&PingPongSuccessful]
    {
      BridgeConnection.Out->Send(json({{"ping",int()}}).dump());
      int reception{0};
      while((reception = BridgeConnection.In->Peek()) == 0)
      {
        std::this_thread::yield();
      }
      try
      {
        if(json::parse(BridgeConnection.In->Copy())["ping"] == 1)
        {
          PingPongSuccessful = 1;
        }
      }catch(...)
      {
        PingPongSuccessful = 0;
      }
    });
    lock.release();
    while(PingPongSuccessful == -1) std::this_thread::yield();
    return PingPongSuccessful == 1;
  }
  else
  {
    return false;
  }
}

void AC::Seeker::BridgeSynchronize(AC::Connector* Instigator,
                                   json Message, bool bFailIfNotResolved)
{
  Message["id"] = Instigator->ID;
  std::string Transmission = Message.dump();
  BridgeConnection.Out->Send(Transmission);
  auto messagelength = BridgeConnection.In->Receive(true);
  if(messagelength <= 0)
  {
    if(bFailIfNotResolved)
    {
      throw std::domain_error(std::string("Could not receive answer from Bridgehead and this synchronization is critical:\n\n")
      + "Message was:\n\n"
      + Message.dump(1,'\t'));
    }
  }
  else
  {
    json Answer;
    try
    {
      Answer = json::parse(BridgeConnection.In->Copy());
    }
    catch(std::exception e)
    {
      if(bFailIfNotResolved)
      {
        throw std::runtime_error(std::string("An error occured while parsing the Bridge response:\n\n")
        + e.what());
      }
    }
    catch(...)
    {
      if(bFailIfNotResolved)
      {
        throw std::exception("An unexpected error occured while parsing the Bridge response");
      }
    }
    Instigator->OnBridgeInformation(Answer);
  }
}

void AC::Seeker::BridgeSubmit(AC::Connector* Instigator, std::variant<std::byte, std::string> Message) const
{
  json Transmission = {{"id",Instigator->ID}};
  // we need to break this up because of json lib compatibility
  if(std::holds_alternative<std::string>(Message))
  {
    Transmission["data"] = std::get<std::string>(Message);
  }
  else
  {
    Transmission["data"] = std::get<std::byte>(Message);
  }
  BridgeConnection.Out->Send(Transmission);
}

void AC::Seeker::BridgeRun()
{
  std::unique_lock<std::mutex> lock(QueueAccess);
  while(true)
  {
    TaskAvaliable.wait(lock, [this]{
            return (CommInstructQueue.size());
        });
    if(CommInstructQueue.size() > 0)
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

void AC::Seeker::Listen()
{
  std::unique_lock<std::mutex> lock(CommandAccess);
  while(true)
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
      auto message = json::parse(this->BridgeConnection.In->Reception);
      std::string type = message["type"];
      auto app_id = message["id"].get<int>();
      UserByID[app_id]->OnBridgeInformation(message);
    } catch( ... )
    {
      
    }
  }
}

void AC::Seeker::FindBridge()
{
  std::unique_lock<std::mutex> lock(QueueAccess);
  lock.lock();
  CommInstructQueue.push([this]()
  {
    
  });
  lock.release();
}

void AC::Seeker::RecoverConnection()
{
}

std::shared_ptr<AC::Connector> AC::Seeker::CreateConnection()
{
  // structural wrapper to forego the need to create a fractured shared pointer
  struct Wrap { Wrap() :cont(AC::Connector()) {} AC::Connector cont; };
  auto t = std::make_shared<Wrap>();
  std::shared_ptr<AC::Connector> Connection{std::move(t),&t->cont };

  Connection->BridgePointer = std::shared_ptr<Seeker>(this);
  Connection->ID = ++NextID;

  Connection->Upstream = std::make_shared<BridgeSocket>
  (std::move(BridgeSocket::GetFreeSocketPort(Config["LocalAddress"])));
  
  return Connection;

}

// this is copied on purpose so that the reference counter should be at least 1
// when entering this method
void AC::Seeker::DestroyConnection(std::shared_ptr<Connector> Connector)
{
  UserByID.erase(Connector->ID);
}

void AC::Seeker::ConfigureUpstream(Connector* Instigator, const json& Answer)
{
  Instigator->Upstream->Address = Config["RemoteAddress"];
  Instigator->Upstream->Port = Config["RemotePort"];
  Instigator->Upstream->Connect();
}

void AC::Seeker::CreateTask(std::function<void(void)>&& Task)
{
  std::unique_lock<std::mutex> lock(QueueAccess);
  lock.lock();
  CommInstructQueue.push(Task);
  lock.unlock();
}


