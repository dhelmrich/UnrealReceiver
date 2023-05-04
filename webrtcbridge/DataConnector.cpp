#include "DataConnector.hpp"
#include <rtc/candidate.hpp>
#include <chrono>
#include <codecvt>
#include <locale>
#include <bit>
#include <fstream>

#ifdef _WIN32
#include <Windows.h>
#endif

#undef min
#undef max

#define LDEBUG if(LogVerbosity >= ELogVerbosity::Debug) std::cout
#define LINFO if(LogVerbosity >= ELogVerbosity::Info) std::cout

inline constexpr std::byte operator "" _b(unsigned long long i) noexcept
{
  return static_cast<std::byte>(i);
}

WebRTCBridge::DataConnector::DataConnector()
{
  PeerConnection = std::make_shared<rtc::PeerConnection>(rtcconfig_);
  PeerConnection->onGatheringStateChange([this](auto state)
    {
      std::cout << Prefix << "Gathering state changed" << state << std::endl;
      switch (state)
      {
      case rtc::PeerConnection::GatheringState::Complete:
        std::cout << Prefix << "State switched to complete" << std::endl;
        break;
      case rtc::PeerConnection::GatheringState::InProgress:
        std::cout << Prefix << "State switched to in progress" << std::endl;
        break;
      case rtc::PeerConnection::GatheringState::New:
        std::cout << Prefix << "State switched to new connection" << std::endl;
        break;
      }
    });
  PeerConnection->onLocalCandidate([this](auto candidate)
    {
      json ice_message = { {"type","iceCandidate"},
        {"candidate", {{"candidate", candidate.candidate()},
                           "sdpMid", candidate.mid()}} };
  SignallingServer->send(ice_message.dump());
    });
  PeerConnection->onDataChannel([this](auto datachannel)
    {
      std::cout << Prefix << "I received a channel I did not ask for" << std::endl;
      datachannel->onOpen([this]()
        {
          std::cout << Prefix << "THEIR DataChannel connection is setup!" << std::endl;
        });
    });
  PeerConnection->onTrack([this](auto track)
    {
      std::cout << Prefix << "I received a track I did not ask for" << std::endl;
      track->onOpen([this, track]()
        {
          std::cout << Prefix << "Track connection is setup!" << std::endl;
          track->send(rtc::binary({ (std::byte)(EClientMessageType::QualityControlOwnership) }));
          track->send(rtc::binary({ std::byte{72},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0} }));
        });
    });
  PeerConnection->onSignalingStateChange([this](auto state)
    {
      std::cout << Prefix << "SS State changed: " << state << std::endl;
    });
  PeerConnection->onStateChange([this](rtc::PeerConnection::State state)
    {
      std::cout << Prefix << "State changed: " << state << std::endl;
      if (state == rtc::PeerConnection::State::Failed && OnFailedCallback.has_value())
      {
        OnFailedCallback.value()();
      }
    });
  SignallingServer = std::make_shared<rtc::WebSocket>();
  DataChannel = PeerConnection->createDataChannel("DataConnectionChannel");
  DataChannel->onOpen([this]()
    {
      std::cout << Prefix << "OUR DataChannel connection is setup!" << std::endl;

    });
  DataChannel->onMessage([this](auto messageordata)
    {
      if (std::holds_alternative<rtc::binary>(messageordata))
      {
        auto data = std::get<rtc::binary>(messageordata);
        // try parse string
        std::string message(reinterpret_cast<char*>(data.data() + 1), data.size() - 1);
        // find readable json subset
        auto first_lbrace = message.find_first_of('{');
        auto last_rbrace = message.find_last_of('}');

        if (first_lbrace < message.length() && last_rbrace < message.length()
          && std::ranges::all_of(message.begin() + first_lbrace, message.begin() + last_rbrace, &isprint))
        {
          if (MessageReceptionCallback.has_value())
            MessageReceptionCallback.value()(message.substr(first_lbrace, last_rbrace - first_lbrace + 1));
        }
        else if (DataReceptionCallback.has_value())
          DataReceptionCallback.value()(data);
      }
      else
      {
        auto message = std::get<std::string>(messageordata);
        if (MessageReceptionCallback.has_value())
          MessageReceptionCallback.value()(message);
      }
    });
  DataChannel->onError([this](std::string error)
    {
      std::cerr << "DataChannel error: " << error << std::endl;
    });
  DataChannel->onAvailable([this]()
    {
      //std::cout << Prefix << "DataChannel is available" << std::endl;
      state_ = EConnectionState::CONNECTED;
      //if (OnConnectedCallback.has_value())
      //{
      //  OnConnectedCallback.value()();
      //}
    });
  DataChannel->onBufferedAmountLow([this]()
    {
      std::cout << Prefix << "DataChannel buffered amount low" << std::endl;
    });
  DataChannel->onClosed([this]()
    {
      std::cout << Prefix << "DataChannel is CLOSED again" << std::endl;
      this->state_ = EConnectionState::CLOSED;
      if (OnClosedCallback.has_value())
      {
        OnClosedCallback.value()();
      }
    });

  SignallingServer->onOpen([this]()
    {
      state_ = EConnectionState::SIGNUP;
      std::cout << Prefix << "Signalling server connected" << std::endl;
      if (TakeFirstStep)
      {
        json role_request = { {"type","request"},{"request","role"} };
        std::cout << Prefix << "Attempting to prompt for role, this will fail if the server is not configured to do so" << std::endl;
        //SignallingServer->send(role_request.dump());
      }
      if (TakeFirstStep && PeerConnection->localDescription().has_value())
      {
        json offer = { {"type","offer"}, {"endpoint", "data"},{"sdp",PeerConnection->localDescription().value()} };
        if (PeerConnection->hasMedia())
        {
          std::cout << "PeerConnection has Media!" << std::endl;
        }
        else
        {
          std::cout << "PeerConnection has no Media!" << std::endl;
        }
        SignallingServer->send(offer.dump());
        state_ = EConnectionState::OFFERED;
      }
    });
  SignallingServer->onMessage([this](auto messageordata)
    {
      if (std::holds_alternative<rtc::binary>(messageordata))
      {
        auto data = std::get<rtc::binary>(messageordata);
      }
      else
      {
        auto message = std::get<std::string>(messageordata);
        json content;
        try
        {
          content = json::parse(message);
        }
        catch (json::exception e)
        {
          std::cout << Prefix << "Could not read package:" << e.what() << std::endl;
        }
        std::cout << Prefix << "I received a message of type: " << content["type"] << std::endl;
        if (content["type"] == "answer" || content["type"] == "offer")
        {
          std::cout << "Received an " << content["type"] << " from the server" << std::endl;
          std::string sdp = content["sdp"].get<std::string>();
          std::string type = content["type"].get<std::string>();
          rtc::Description remote(sdp, type);
          if (content["type"] == "answer" && TakeFirstStep)
            PeerConnection->setRemoteDescription(remote);
          else if (content["type"] == "offer" && !TakeFirstStep)
          {
            PeerConnection->setRemoteDescription(remote);
            SubmissionHandler.AddTask(std::bind(&DataConnector::CommunicateSDPs, this));
          }
          if (!InitializedRemote)
          {
            RequiredCandidate.clear();
            // iterating through media sections in the descriptions
            for (unsigned i = 0; i < remote.mediaCount(); ++i)
            {
              auto extract = remote.media(i);
              if (std::holds_alternative<rtc::Description::Application*>(extract))
              {
                auto app = std::get<rtc::Description::Application*>(extract);
                RequiredCandidate.push_back(app->mid());
              }
              else
              {
                auto media = std::get<rtc::Description::Media*>(extract);
                RequiredCandidate.push_back(media->mid());
              }
            }
            std::cout << Prefix << "I have " << RequiredCandidate.size() << " required candidates: ";
            for (auto i = 0; i < RequiredCandidate.size(); ++i)
            {
              std::cout << Prefix << RequiredCandidate[i] << " ";
            }
            std::cout << Prefix << std::endl;
            InitializedRemote = true;
          }
        }
        else if (content["type"] == "iceCandidate")
        {
          // {"type": "iceCandidate", "candidate": {"candidate": "candidate:1 1 UDP 2122317823 172.26.15.227 42835 typ host", "sdpMLineIndex": "0", "sdpMid": "0"}}
          std::cout << Prefix << "Parsing" << std::endl;
          std::string sdpMid, candidate_string;
          int sdpMLineIndex;
          try
          {
            sdpMid = content["candidate"]["sdpMid"].get<std::string>();
            sdpMLineIndex = content["candidate"]["sdpMLineIndex"].get<int>();
            candidate_string = content["candidate"]["candidate"].get<std::string>();
          }
          catch (std::exception e)
          {
            std::cout << Prefix << "Could not parse candidate: " << e.what() << std::endl;
            return;
          }
          std::cout << Prefix << "I received a candidate for " << sdpMid << " with index " << sdpMLineIndex << " and candidate " << candidate_string << std::endl;
          rtc::Candidate ice(candidate_string, sdpMid);
          try
          {
            PeerConnection->addRemoteCandidate(ice);
            // remove from required candidates if succeeded
            RequiredCandidate.erase(std::remove_if(RequiredCandidate.begin(), RequiredCandidate.end(), [sdpMid](auto s) {return s == sdpMid; }), RequiredCandidate.end());
          }
          catch (std::exception e)
          {
            std::cout << Prefix << "Could not add remote candidate: " << e.what() << std::endl;
          }
          // if we have no more required candidates, we can send an answer
          if (RequiredCandidate.size() == 0)
          {
            std::cout << Prefix << "I have received all required candidates" << std::endl;
            if (!TakeFirstStep && PeerConnection->localDescription().has_value() && state_ < EConnectionState::OFFERED)
            {
              this->state_ = EConnectionState::OFFERED;
              SubmissionHandler.AddTask(std::bind(&DataConnector::CommunicateSDPs, this));
            }
            if (OnIceGatheringFinished.has_value())
            {
              OnIceGatheringFinished.value()();

            }
          }
          else
          {
            std::cout << Prefix << "I still have " << RequiredCandidate.size() << " required candidates: ";
            for (auto i = 0; i < RequiredCandidate.size(); ++i)
            {
              std::cout << Prefix << RequiredCandidate[i] << " ";
            }
            std::cout << Prefix << std::endl;
          }
        }
        else if (content["type"] == "control")
        {
          std::cout << Prefix << "Received a control message: " << content["message"] << std::endl;
        }
        else if (content["type"] == "id")
        {
          this->config_["id"] = content["id"];
          std::cout << Prefix << "Received an id: " << content["id"] << std::endl;
        }
        else if (content["type"] == "serverDisconnected")
        {
          PeerConnection.reset();
          std::cout << Prefix << "Reset peer connection because we received a disconnect" << std::endl;
        }
        else if (content["type"] == "config")
        {
          auto pc_options = content["peerConnectionOptions"];
          // TODO: Set peer connection options
        }
        else if (content["type"] == "playerCount")
        {
          std::cout << Prefix << "There are " << content["count"] << " players connected" << std::endl;
        }
        else if (content["type"] == "role")
        {
          std::cout << Prefix << "Received a role: " << content["role"] << std::endl;
          if (content["role"] == "server")
          {
            this->IsServer = true;
            PeerConnection->setLocalDescription();
          }
        }
        else if (content["type"] == "playerConnected")
        {
          json offer = { {"type","offer"}, {"endpoint", "data"},{"sdp",PeerConnection->localDescription().value()} };
          SignallingServer->send(offer.dump());
          SubmissionHandler.AddTask(std::bind(&DataConnector::CommunicateSDPs, this));
        }
        else if (content["type"] == "playerDisconnected")
        {
          PeerConnection.reset();
          std::cout << Prefix << "Resetting connection because we must be in server role and the player disconnected" << std::endl;
        }
        else
        {
          std::cout << Prefix << "unknown message?" << std::endl << content.dump() << std::endl;
        }
      }
    });
  SignallingServer->onClosed([this]()
    {
      state_ = EConnectionState::CLOSED;
      auto unix_time = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

      std::cout << Prefix << "Signalling server was closed at timestamp " << unix_time << std::endl;

    });
  SignallingServer->onError([this](std::string error)
    {
      state_ = EConnectionState::STARTUP;
      SignallingServer->close();
      std::cerr << "Signalling server error: " << error << std::endl;
    });
}

WebRTCBridge::DataConnector::~DataConnector()
{
  SignallingServer->close();
  PeerConnection->close();
  while (SignallingServer->readyState() != rtc::WebSocket::State::Closed)
    std::this_thread::yield();
}

void WebRTCBridge::DataConnector::StartSignalling()
{
  std::string address = "ws://" + config_["SignallingIP"].get<std::string>()
    + ":" + std::to_string(config_["SignallingPort"].get<unsigned>());
  LINFO << Prefix << "Starting Signalling to " << address << std::endl;
  state_ = EConnectionState::STARTUP;
  SignallingServer->open(address);
  while (Block && state_ < EConnectionState::SIGNUP)
  {
    std::this_thread::yield();
  }
}

void WebRTCBridge::DataConnector::SendData(rtc::binary Data)
{
  if (this->state_ != EConnectionState::CONNECTED)
    return;
  if (Data.size() > DataChannel->maxMessageSize())
  {
    rtc::binary Chunk(DataChannel->maxMessageSize());
    const auto meta_size = sizeof(int) + sizeof(int) + sizeof(uint16_t) + sizeof(std::byte);
    unsigned int chunks{ 0 };
    for (; (meta_size * chunks + Data.size()) / chunks < DataChannel->maxMessageSize(); ++chunks);

    // iterate through the chunks
    for (auto i = 0u; i < chunks; ++i)
    {
      unsigned int n = 0u;
      n = InsertIntoBinary(Chunk, n, std::byte(50), uint16_t(0));
      n = InsertIntoBinary(Chunk, n, i, chunks);
      memcpy(Chunk.data() + n, Data.data() + i * chunks, DataChannel->maxMessageSize() - meta_size);
      DataChannel->sendBuffer(Chunk);
    }
  }
  else
  {
    DataChannel->sendBuffer(Data);
  }
}

void WebRTCBridge::DataConnector::SendString(std::string Message)
{
  if (this->state_ != EConnectionState::CONNECTED)
    return;
  json content = { {"origin","dataconnector"},{"data",Message} };
  std::string json_message = content.dump();
  // prepare bytes that Unreal expects at the beginning of the message
  // rtc::binary bytes(3 + 2 * json_message.length());
  rtc::binary bytes(4 + json_message.length());
  bytes[bytes.size() - 1] = std::byte(0); // null terminator
  bytes.at(0) = DataChannelByte;
  uint16_t* buffer = reinterpret_cast<uint16_t*>(&(bytes.at(1)));
  *buffer = static_cast<uint16_t>(json_message.size());
  for (int i = 0; i < json_message.size(); i++)
  {
    //bytes.at(3 + 2 * i) = static_cast<std::byte>(json_message.at(i));
    //bytes.at(3 + 2 * i + 1) = 0_b;
    bytes.at(3 + i) = static_cast<std::byte>(json_message.at(i));
  }
  DataChannel->sendBuffer(bytes);
}

void WebRTCBridge::DataConnector::SendJSON(json Message)
{
  if (this->state_ != EConnectionState::CONNECTED)
    return;
  std::string json_message = Message.dump();
  // prepare bytes that Unreal expects at the beginning of the message
  rtc::binary bytes(4 + json_message.length());
  bytes[bytes.size() - 1] = std::byte(0);
  bytes.at(0) = DataChannelByte;
  uint16_t* buffer = reinterpret_cast<uint16_t*>(&(bytes.at(1)));
  *buffer = static_cast<uint16_t>(json_message.size());

  // copy the json string into the buffer
  memcpy(bytes.data() + 3, json_message.data(), json_message.size());
  
  std::string temp = std::string((char*)bytes.data(), bytes.size());
  LINFO << "Sending JSON: " << temp << std::endl;
  DataChannel->sendBuffer(bytes);
}

void WebRTCBridge::DataConnector::SendBuffer(const std::span<const uint8_t>& Buffer, std::string Name, std::string Format)
{
  // this method briefly exchanges the callback for message reception
  auto msg_callback = MessageReceptionCallback;
  int MessageState = -1;
  int StateTracker = 0;
  this->SetMessageCallback([&msg_callback, &MessageState, this](std::string Message)
  {
    std::cout << "Message received: " << Message << std::endl;
    json content = json::parse(Message);
    if (content["type"] == "buffer")
    {
      MessageState = MessageState + 1;
    }
    else if (msg_callback.has_value())
    {
      msg_callback.value()(Message);
    }
  });
  std::size_t chunk_size{}, chunks{}, total_size{};
  const uint8_t* Source = nullptr;
  bool NeedToDelete = false;
  if (Format == "raw")
  {
    total_size = Buffer.size();
    chunk_size = DataChannel->maxMessageSize() - 4;
    chunks = std::max(Buffer.size() / chunk_size,static_cast<std::size_t>(1));
    Source = reinterpret_cast<const uint8_t*>(Buffer.data());
  }
  else if (Format == "base64")
  {
    total_size = EncodedSize(Buffer);
    chunk_size = DataChannel->maxMessageSize() - 4;
    chunks = std::max(total_size / chunk_size, static_cast<std::size_t>(1));
    const auto ConvertedDat = Encode64(Buffer);
    const std::string DebugTest(ConvertedDat.begin(), ConvertedDat.end());
    Source = reinterpret_cast<const uint8_t*>(ConvertedDat.data());
    NeedToDelete = true;
  }
  else if (Format == "ascii")
  {

  }
  if (!Source)
  {
    std::cout << Prefix << "Invalid format for buffer transmission" << std::endl;
    return;
  }
  // transmit
  LDEBUG << Prefix << "Transmitting buffer of size " << Buffer.size() << " in " << chunks << " chunks of size " << chunk_size << std::endl;
  this->SendJSON({ {"type","buffer"}, {"start",Name }, {"size", total_size}, {"format", Format} });
  LDEBUG << Prefix << "Sent start message" << std::endl;
  while (MessageState < StateTracker) { std::this_thread::yield(); }
  StateTracker++;
  LDEBUG << Prefix << "Received start message" << std::endl;
  rtc::binary bytes(std::min(chunk_size, total_size) + 4);
  bytes.at(bytes.size() - 1) = std::byte(0);
  uint8_t* buffer = reinterpret_cast<uint8_t*>(&(bytes.at(3)));
  bytes.at(0) = DataChannelByte;
  // move through the chunks
  for (int i = 0; i < chunks; i++)
  {
    const auto remaining = std::min(chunk_size, total_size - i * chunk_size);
    if(bytes.size() > remaining + 4)
    {
      bytes.resize(remaining + 4);
      bytes.at(bytes.size() - 1) = std::byte(0);
    }
    // copy the chunk into the buffer, the std::min is to avoid copying too much
    memcpy(buffer, Source + i * chunk_size, remaining);
    // set the second and third bytes to the chunk size
    *(reinterpret_cast<uint16_t*>(&(bytes.at(1)))) = remaining;
    // send the buffer
    DataChannel->sendBuffer(bytes);
    LDEBUG << Prefix << "Sent chunk " << i << " of length " << remaining << std::endl;
    // wait for the message to be received
    while (MessageState < StateTracker) { std::this_thread::yield(); }
    LDEBUG << Prefix << "Received message " << i << std::endl;
    StateTracker++;
  }
  this->SendJSON({ {"type","buffer"},{"stop",Name} });
  while (MessageState < StateTracker) { std::this_thread::yield(); }
  LDEBUG << Prefix << "Sent stop message" << std::endl;
  // restore the original callback
  MessageReceptionCallback = msg_callback;
  if(NeedToDelete)
  {
    delete[] Source;
  }
}

void WebRTCBridge::DataConnector::SendFloat64Buffer(const std::vector<double>& Buffer, std::string Name, std::string Format)
{
  this->SendBuffer(std::span(reinterpret_cast<const uint8_t*>(Buffer.data()), Buffer.size() * sizeof(double)), Name, Format);
}

void WebRTCBridge::DataConnector::SendFloat32Buffer(const std::vector<float>& Buffer, std::string Name, std::string Format)
{
  this->SendBuffer(std::span(reinterpret_cast<const uint8_t*>(Buffer.data()), Buffer.size() * sizeof(float)), Name, Format);
}

void WebRTCBridge::DataConnector::SendInt32Buffer(const std::vector<int32_t>& Buffer, std::string Name,
  std::string Format)
{
  this->SendBuffer(std::span(reinterpret_cast<const uint8_t*>(Buffer.data()), Buffer.size() * sizeof(int32_t)), Name, Format);
}

void WebRTCBridge::DataConnector::SendGeometry(const std::vector<double>& Vertices, const std::vector<uint32_t>& Indices,
                                               const std::vector<double>& Normals, std::string Name, std::optional<std::vector<double>> UVs,
                                               std::optional<std::vector<double>> Tangents)
{
  json Message = { {"type","geometry"},{"name",Name} };
  // calculate the total size[bytes] of the message if we were to send it as a single buffer
  std::size_t total_size = 3;
  // because a single buffer would mean that we use JSON, we need to add the base64 size of the buffers
  total_size += EncodedSize(Vertices);
  total_size += EncodedSize(Indices);
  total_size += EncodedSize(Normals);
  if (UVs.has_value())
  {
    total_size += EncodedSize(UVs.value());
  }
  if (Tangents.has_value())
  {
    total_size += EncodedSize(Tangents.value());
  }
  // check if we can send the message as a single buffer
  if (total_size < this->DataChannel->maxMessageSize())
  {
    Message["type"] = "directbase64";
    Message["vertices"] = Encode64(Vertices);
    Message["indices"] = Encode64(Indices);
    Message["normals"] = Encode64(Normals);
    if (UVs.has_value())
    {
      Message["uvs"] = Encode64(UVs.value());
    }
    if (Tangents.has_value())
    {
      Message["tangents"] = Encode64(Tangents.value());
    }
    this->SendJSON(Message);
  }
  else
  {
    // we need to send the message in chunks
    Message["state"] = "start";
    std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(Vertices.data()), reinterpret_cast<const uint8_t*>(Vertices.data() + Vertices.size()));
    
    this->SendJSON(Message);
    // send the vertices
    this->SendBuffer(data, "points", "base64");
    // send the indices
    data = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(Indices.data()), Indices.size() * sizeof(float));
    this->SendBuffer(data, "triangles", "base64");
    // send the normals
    data = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(Normals.data()), Normals.size() * sizeof(float));
    this->SendBuffer(data, "normals", "base64");
    // send the UVs
    if (UVs.has_value())
    {
      data = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(UVs.value().data()), UVs.value().size() * sizeof(float));
      this->SendBuffer(data, "uvs", "base64");
    }
    // send the tangents
    if (Tangents.has_value())
    {
      data = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(Tangents.value().data()), Tangents.value().size() * sizeof(float));
      this->SendBuffer(data, "tangents", "base64");
    }
    // send the stop message
    Message["state"] = "stop";
    this->SendJSON(Message);
  }
}

WebRTCBridge::EConnectionState WebRTCBridge::DataConnector::GetState()
{
  return state_;
}

void WebRTCBridge::DataConnector::SetDataCallback(std::function<void(rtc::binary)> Callback)
{
  this->DataReceptionCallback = Callback;
}

void WebRTCBridge::DataConnector::SetMessageCallback(std::function<void(std::string)> Callback)
{
  this->MessageReceptionCallback = Callback;
}

void WebRTCBridge::DataConnector::SetConfigFile(std::string ConfigFile)
{
  std::ifstream file(ConfigFile);
  if (!file.is_open())
  {
    std::cerr << "Could not open config file " << ConfigFile << std::endl;
    return;
  }
  json js = json::parse(file);
  SetConfig(js);
}

void WebRTCBridge::DataConnector::SetConfig(json Config)
{
  bool all_found = true;
  // use items iterator for config to check if all required values are present
  for (auto& [key, value] : config_.items())
  {
    if (Config.find(key) == Config.end())
    {
      all_found = false;
      break;
    }
  }
  if (!all_found)
  {
    std::cerr << "Config is missing required values" << std::endl;
    std::cerr << "Required values are: " << std::endl;
    for (auto& [key, value] : config_.items())
    {
      std::cerr << key << " ";
    }
    std::cerr << std::endl << "Provided values are: " << std::endl;
    for (auto& [key, value] : Config.items())
    {
      std::cerr << key << " ";
    }
    throw std::runtime_error("Config is missing required values");
  }
  // inserting all values from config into config_
  // this is done to ensure that all required values are present
  for (auto& [key, value] : Config.items())
  {
    config_[key] = value;
  }

}

bool WebRTCBridge::DataConnector::IsRunning()
{
  // returns true if the connection is in a state where it can send and receive data
  return state_ < EConnectionState::CLOSED || SignallingServer->isOpen();

}

// a method that outputs data channel information
void WebRTCBridge::DataConnector::PrintCommunicationData()
{
  auto max_message = DataChannel->maxMessageSize();
  auto protocol = DataChannel->protocol();
  auto label = DataChannel->label();
  std::cout << Prefix << "Data Channel " << label << " has protocol " << protocol << " and max message size " << max_message << std::endl;

}

void WebRTCBridge::DataConnector::CommunicateSDPs()
{
  if (PeerConnection->localDescription().has_value())
  {
    if (!TakeFirstStep)
    {
      json offer = { {"type","answer"}, {"sdp",PeerConnection->localDescription().value()} };
      SignallingServer->send(offer.dump());
    }
    for (auto candidate : PeerConnection->localDescription().value().extractCandidates())
    {
      json ice = { {"type","iceCandidate"}, {"candidate", {{"candidate",candidate.candidate()}, {"sdpMid",candidate.mid()}, {"sdpMLineIndex",std::stoi(candidate.mid())}}} };
      SignallingServer->send(ice.dump());
    }
  }
}
