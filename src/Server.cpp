// ReDucTor is an awesome guy who helped me a lot

#include "Globals.h"  // NOTE: MSVC stupidness requires this to be the same across all modules

#include "Server.h"
#include "ClientHandle.h"
#include "Mobs/Monster.h"
#include "OSSupport/Socket.h"
#include "Root.h"
#include "World.h"
#include "ChunkDef.h"
#include "Bindings/PluginManager.h"
#include "ChatColor.h"
#include "Entities/Player.h"
#include "Inventory.h"
#include "Item.h"
#include "FurnaceRecipe.h"
#include "WebAdmin.h"
#include "Protocol/ProtocolRecognizer.h"
#include "CommandOutput.h"

#include "IniFile.h"
#include "Vector3.h"

#include <fstream>
#include <sstream>
#include <iostream>

extern "C"
{
	#include "zlib/zlib.h"
}




// For the "dumpmem" server command:
/// Synchronize this with main.cpp - the leak finder needs initialization before it can be used to dump memory
// _X 2014_02_20: Disabled for canon repo, it makes the debug version too slow in MSVC2013
// and we haven't had a memory leak for over a year anyway.
// #define ENABLE_LEAK_FINDER

#if defined(_MSC_VER) && defined(_DEBUG) && defined(ENABLE_LEAK_FINDER)
	#pragma warning(push)
	#pragma warning(disable:4100)
	#include "LeakFinder.h"
	#pragma warning(pop)
#endif





typedef std::list< cClientHandle* > ClientList;





////////////////////////////////////////////////////////////////////////////////
// cServer::cTickThread:

cServer::cTickThread::cTickThread(cServer & a_Server) :
	super("ServerTickThread"),
	m_Server(a_Server)
{
}





void cServer::cTickThread::Execute(void)
{
	auto LastTime = std::chrono::steady_clock::now();
	static const auto msPerTick = std::chrono::milliseconds(50);

	while (!m_ShouldTerminate)
	{
		auto NowTime = std::chrono::steady_clock::now();
		auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(NowTime - LastTime).count();
		m_ShouldTerminate = !m_Server.Tick(static_cast<float>(msec));
		auto TickTime = std::chrono::steady_clock::now() - NowTime;
		
		if (TickTime < msPerTick)
		{
			// Stretch tick time until it's at least msPerTick
			std::this_thread::sleep_for(msPerTick - TickTime);
		}

		LastTime = NowTime;
	}
}





////////////////////////////////////////////////////////////////////////////////
// cServer:

cServer::cServer(void) :
	m_ListenThreadIPv4(*this, cSocket::IPv4, "Client"),
	m_ListenThreadIPv6(*this, cSocket::IPv6, "Client"),
	m_PlayerCount(0),
	m_PlayerCountDiff(0),
	m_ClientViewDistance(0),
	m_bIsConnected(false),
	m_bRestarting(false),
	m_RCONServer(*this),
	m_MaxPlayers(0),
	m_bIsHardcore(false),
	m_TickThread(*this),
	m_ShouldAuthenticate(false),
	m_ShouldLoadOfflinePlayerData(false),
	m_ShouldLoadNamedPlayerData(true)
{
}





void cServer::ClientDestroying(const cClientHandle * a_Client)
{
	m_SocketThreads.RemoveClient(a_Client);
}





void cServer::NotifyClientWrite(const cClientHandle * a_Client)
{
	m_NotifyWriteThread.NotifyClientWrite(a_Client);
}





void cServer::WriteToClient(const cClientHandle * a_Client, const AString & a_Data)
{
	m_SocketThreads.Write(a_Client, a_Data);
}





void cServer::RemoveClient(const cClientHandle * a_Client)
{
	m_SocketThreads.RemoveClient(a_Client);
}





void cServer::ClientMovedToWorld(const cClientHandle * a_Client)
{
	cCSLock Lock(m_CSClients);
	m_ClientsToRemove.push_back(const_cast<cClientHandle *>(a_Client));
}





void cServer::PlayerCreated(const cPlayer * a_Player)
{
	UNUSED(a_Player);
	// To avoid deadlocks, the player count is not handled directly, but rather posted onto the tick thread
	cCSLock Lock(m_CSPlayerCountDiff);
	m_PlayerCountDiff += 1;
}





void cServer::PlayerDestroying(const cPlayer * a_Player)
{
	UNUSED(a_Player);
	// To avoid deadlocks, the player count is not handled directly, but rather posted onto the tick thread
	cCSLock Lock(m_CSPlayerCountDiff);
	m_PlayerCountDiff -= 1;
}





bool cServer::InitServer(cIniFile & a_SettingsIni, bool a_ShouldAuth)
{
	m_Description = a_SettingsIni.GetValueSet("Server", "Description", "MCServer - in C++!");
	m_MaxPlayers  = a_SettingsIni.GetValueSetI("Server", "MaxPlayers", 100);
	m_bIsHardcore = a_SettingsIni.GetValueSetB("Server", "HardcoreEnabled", false);
	m_bAllowMultiLogin = a_SettingsIni.GetValueSetB("Server", "AllowMultiLogin", false);
	m_PlayerCount = 0;
	m_PlayerCountDiff = 0;

	m_FaviconData = Base64Encode(cFile::ReadWholeFile(FILE_IO_PREFIX + AString("favicon.png")));  // Will return empty string if file nonexistant; client doesn't mind

	if (m_bIsConnected)
	{
		LOGERROR("ERROR: Trying to initialize server while server is already running!");
		return false;
	}

	LOGINFO("Compatible clients: %s", MCS_CLIENT_VERSIONS);
	LOGINFO("Compatible protocol versions %s", MCS_PROTOCOL_VERSIONS);

	if (cSocket::WSAStartup() != 0)  // Only does anything on Windows, but whatever
	{
		LOGERROR("WSAStartup() != 0");
		return false;
	}

	bool HasAnyPorts = false;
	AString Ports = a_SettingsIni.GetValueSet("Server", "Port", "25565");
	m_ListenThreadIPv4.SetReuseAddr(true);
	if (m_ListenThreadIPv4.Initialize(Ports))
	{
		HasAnyPorts = true;
	}

	Ports = a_SettingsIni.GetValueSet("Server", "PortsIPv6", "25565");
	m_ListenThreadIPv6.SetReuseAddr(true);
	if (m_ListenThreadIPv6.Initialize(Ports))
	{
		HasAnyPorts = true;
	}
	
	if (!HasAnyPorts)
	{
		LOGERROR("Couldn't open any ports. Aborting the server");
		return false;
	}

	m_RCONServer.Initialize(a_SettingsIni);

	m_bIsConnected = true;

	m_ServerID = "-";
	m_ShouldAuthenticate = a_ShouldAuth;
	if (m_ShouldAuthenticate)
	{
		MTRand mtrand1;
		unsigned int r1 = (mtrand1.randInt() % 1147483647) + 1000000000;
		unsigned int r2 = (mtrand1.randInt() % 1147483647) + 1000000000;
		std::ostringstream sid;
		sid << std::hex << r1;
		sid << std::hex << r2;
		m_ServerID = sid.str();
		m_ServerID.resize(16, '0');
	}

	// Check if both BungeeCord and online mode are on, if so, warn the admin:
	m_ShouldAllowBungeeCord = a_SettingsIni.GetValueSetB("Authentication", "AllowBungeeCord", false);
	if (m_ShouldAllowBungeeCord && m_ShouldAuthenticate)
	{
		LOGWARNING("WARNING: BungeeCord is allowed and server set to online mode. This is unsafe and will not work properly. Disable either authentication or BungeeCord in settings.ini.");
	}
	
	m_ShouldLoadOfflinePlayerData = a_SettingsIni.GetValueSetB("PlayerData", "LoadOfflinePlayerData", false);
	m_ShouldLoadNamedPlayerData   = a_SettingsIni.GetValueSetB("PlayerData", "LoadNamedPlayerData", true);
	
	m_ClientViewDistance = a_SettingsIni.GetValueSetI("Server", "DefaultViewDistance", cClientHandle::DEFAULT_VIEW_DISTANCE);
	if (m_ClientViewDistance < cClientHandle::MIN_VIEW_DISTANCE)
	{
		m_ClientViewDistance = cClientHandle::MIN_VIEW_DISTANCE;
		LOGINFO("Setting default viewdistance to the minimum of %d", m_ClientViewDistance);
	}
	if (m_ClientViewDistance > cClientHandle::MAX_VIEW_DISTANCE)
	{
		m_ClientViewDistance = cClientHandle::MAX_VIEW_DISTANCE;
		LOGINFO("Setting default viewdistance to the maximum of %d", m_ClientViewDistance);
	}
	
	m_NotifyWriteThread.Start(this);
	
	PrepareKeys();
	
	return true;
}





int cServer::GetNumPlayers(void) const
{
	cCSLock Lock(m_CSPlayerCount);
	return m_PlayerCount;
}





bool cServer::IsPlayerInQueue(AString a_Username)
{
	cCSLock Lock(m_CSClients);
	for (auto client : m_Clients)
	{
		if ((client->GetUsername()).compare(a_Username) == 0)
		{
			return true;
		}
	}
	return false;
}





void cServer::PrepareKeys(void)
{
	LOGD("Generating protocol encryption keypair...");
	VERIFY(m_PrivateKey.Generate(1024));
	m_PublicKeyDER = m_PrivateKey.GetPubKeyDER();
}





void cServer::OnConnectionAccepted(cSocket & a_Socket)
{
	if (!a_Socket.IsValid())
	{
		return;
	}
	
	const AString & ClientIP = a_Socket.GetIPString();
	if (ClientIP.empty())
	{
		LOGWARN("cServer: A client connected, but didn't present its IP, disconnecting.");
		a_Socket.CloseSocket();
		return;
	}

	LOGD("Client \"%s\" connected!", ClientIP.c_str());

	cClientHandle * NewHandle = new cClientHandle(&a_Socket, m_ClientViewDistance);
	if (!m_SocketThreads.AddClient(a_Socket, NewHandle))
	{
		// For some reason SocketThreads have rejected the handle, clean it up
		LOGERROR("Client \"%s\" cannot be handled, server probably unstable", ClientIP.c_str());
		a_Socket.CloseSocket();
		delete NewHandle;
		NewHandle = nullptr;
		return;
	}
	
	cCSLock Lock(m_CSClients);
	m_Clients.push_back(NewHandle);
}





bool cServer::Tick(float a_Dt)
{
	// Apply the queued playercount adjustments (postponed to avoid deadlocks)
	int PlayerCountDiff = 0;
	{
		cCSLock Lock(m_CSPlayerCountDiff);
		std::swap(PlayerCountDiff, m_PlayerCountDiff);
	}
	{
		cCSLock Lock(m_CSPlayerCount);
		m_PlayerCount += PlayerCountDiff;
	}
	
	// Send the tick to the plugins, as well as let the plugin manager reload, if asked to (issue #102):
	cPluginManager::Get()->Tick(a_Dt);
	
	// Let the Root process all the queued commands:
	cRoot::Get()->TickCommands();
	
	// Tick all clients not yet assigned to a world:
	TickClients(a_Dt);

	if (!m_bRestarting)
	{
		return true;
	}
	else
	{
		m_bRestarting = false;
		m_RestartEvent.Set();
		return false;
	}
}





void cServer::TickClients(float a_Dt)
{
	cClientHandleList RemoveClients;
	{
		cCSLock Lock(m_CSClients);
		
		// Remove clients that have moved to a world (the world will be ticking them from now on)
		for (cClientHandleList::const_iterator itr = m_ClientsToRemove.begin(), end = m_ClientsToRemove.end(); itr != end; ++itr)
		{
			m_Clients.remove(*itr);
		}  // for itr - m_ClientsToRemove[]
		m_ClientsToRemove.clear();
		
		// Tick the remaining clients, take out those that have been destroyed into RemoveClients
		for (cClientHandleList::iterator itr = m_Clients.begin(); itr != m_Clients.end();)
		{
			if ((*itr)->IsDestroyed())
			{
				// Remove the client later, when CS is not held, to avoid deadlock: http://forum.mc-server.org/showthread.php?tid=374
				RemoveClients.push_back(*itr);
				itr = m_Clients.erase(itr);
				continue;
			}
			(*itr)->ServerTick(a_Dt);
			++itr;
		}  // for itr - m_Clients[]
	}
	
	// Delete the clients that have been destroyed
	for (cClientHandleList::iterator itr = RemoveClients.begin(); itr != RemoveClients.end(); ++itr)
	{
		delete *itr;
	}  // for itr - RemoveClients[]
}





bool cServer::Start(void)
{
	if (!m_ListenThreadIPv4.Start())
	{
		return false;
	}
	if (!m_ListenThreadIPv6.Start())
	{
		return false;
	}
	if (!m_TickThread.Start())
	{
		return false;
	}
	return true;
}





bool cServer::Command(cClientHandle & a_Client, AString & a_Cmd)
{
	return cRoot::Get()->GetPluginManager()->CallHookChat(*(a_Client.GetPlayer()), a_Cmd);
}





void cServer::ExecuteConsoleCommand(const AString & a_Cmd, cCommandOutputCallback & a_Output)
{
	AStringVector split = StringSplit(a_Cmd, " ");
	if (split.empty())
	{
		return;
	}

	// "stop" and "restart" are handled in cRoot::ExecuteConsoleCommand, our caller, due to its access to controlling variables
	
	// "help" and "reload" are to be handled by MCS, so that they work no matter what
	if (split[0] == "help")
	{
		PrintHelp(split, a_Output);
		a_Output.Finished();
		return;
	}
	else if (split[0] == "reload")
	{
		cPluginManager::Get()->ReloadPlugins();
		a_Output.Finished();
		return;
	}
	else if (split[0] == "reloadplugins")
	{
		cPluginManager::Get()->ReloadPlugins();
		a_Output.Out("Plugins reloaded");
		a_Output.Finished();
		return;
	}
	else if (split[0] == "load")
	{
		if (split.size() > 1)
		{
			cPluginManager::PluginMap map = cPluginManager::Get()->GetAllPlugins();

			for (auto plugin_entry : map)
			{
				if (plugin_entry.first == split[1])
				{
					a_Output.Out("Error! Plugin is already loaded!");
					a_Output.Finished();
					return;
				}
			}
			a_Output.Out(cPluginManager::Get()->LoadPlugin(split[1]) ? "Plugin loaded" : "Error occurred loading plugin");
		}
		else
		{
			a_Output.Out("Usage: load <pluginname>");
		}
		a_Output.Finished();
		return;
	}
	else if (split[0] == "unload")
	{
		if (split.size() > 1)
		{
			cPluginManager::Get()->RemovePlugin(cPluginManager::Get()->GetPlugin(split[1]));
			a_Output.Out("Plugin unloaded");
		}
		else
		{
			a_Output.Out("Usage: unload <pluginname>");
		}
		a_Output.Finished();
		return;
	}
	if (split[0] == "destroyentities")
	{
		class WorldCallback : public cWorldListCallback
		{
			virtual bool Item(cWorld * a_World) override
			{
				class EntityCallback : public cEntityCallback
				{
					virtual bool Item(cEntity * a_Entity) override
					{
						if (!a_Entity->IsPlayer())
						{
							a_Entity->Destroy();
						}
						return false;
					}
				} EC;
				a_World->ForEachEntity(EC);
				return false;
			}
		} WC;
		cRoot::Get()->ForEachWorld(WC);
		a_Output.Out("Destroyed all entities");
		a_Output.Finished();
		return;
	}

	// There is currently no way a plugin can do these (and probably won't ever be):
	else if (split[0].compare("chunkstats") == 0)
	{
		cRoot::Get()->LogChunkStats(a_Output);
		a_Output.Finished();
		return;
	}
	#if defined(_MSC_VER) && defined(_DEBUG) && defined(ENABLE_LEAK_FINDER)
	else if (split[0].compare("dumpmem") == 0)
	{
		LeakFinderXmlOutput Output("memdump.xml");
		DumpUsedMemory(&Output);
		return;
	}
	
	else if (split[0].compare("killmem") == 0)
	{
		for (;;)
		{
			new char[100 * 1024 * 1024];  // Allocate and leak 100 MiB in a loop -> fill memory and kill MCS
		}
	}
	#endif

	else if (cPluginManager::Get()->ExecuteConsoleCommand(split, a_Output))
	{
		a_Output.Finished();
		return;
	}
	
	a_Output.Out("Unknown command, type 'help' for all commands.");
	a_Output.Finished();
}





void cServer::PrintHelp(const AStringVector & a_Split, cCommandOutputCallback & a_Output)
{
	UNUSED(a_Split);
	typedef std::pair<AString, AString> AStringPair;
	typedef std::vector<AStringPair> AStringPairs;
	
	class cCallback :
		public cPluginManager::cCommandEnumCallback
	{
	public:
		cCallback(void) : m_MaxLen(0) {}
		
		virtual bool Command(const AString & a_Command, const cPlugin * a_Plugin, const AString & a_Permission, const AString & a_HelpString) override
		{
		UNUSED(a_Plugin);
		UNUSED(a_Permission);
			if (!a_HelpString.empty())
			{
				m_Commands.push_back(AStringPair(a_Command, a_HelpString));
				if (m_MaxLen < a_Command.length())
				{
					m_MaxLen = a_Command.length();
				}
			}
			return false;
		}
		
		AStringPairs m_Commands;
		size_t m_MaxLen;
	} Callback;
	cPluginManager::Get()->ForEachConsoleCommand(Callback);
	std::sort(Callback.m_Commands.begin(), Callback.m_Commands.end());
	for (AStringPairs::const_iterator itr = Callback.m_Commands.begin(), end = Callback.m_Commands.end(); itr != end; ++itr)
	{
		const AStringPair & cmd = *itr;
		a_Output.Out(Printf("%-*s%s\n", static_cast<int>(Callback.m_MaxLen), cmd.first.c_str(), cmd.second.c_str()));
	}  // for itr - Callback.m_Commands[]
	a_Output.Finished();
}





void cServer::BindBuiltInConsoleCommands(void)
{
	cPluginManager * PlgMgr = cPluginManager::Get();
	PlgMgr->BindConsoleCommand("help", nullptr, " - Shows the available commands");
	PlgMgr->BindConsoleCommand("reload", nullptr, " - Reloads all plugins");
	PlgMgr->BindConsoleCommand("restart", nullptr, " - Restarts the server cleanly");
	PlgMgr->BindConsoleCommand("stop", nullptr, " - Stops the server cleanly");
	PlgMgr->BindConsoleCommand("chunkstats", nullptr, " - Displays detailed chunk memory statistics");
	PlgMgr->BindConsoleCommand("load <pluginname>", nullptr, " - Adds and enables the specified plugin");
	PlgMgr->BindConsoleCommand("unload <pluginname>", nullptr, " - Disables the specified plugin");
	PlgMgr->BindConsoleCommand("destroyentities", nullptr, " - Destroys all entities in all worlds");

	#if defined(_MSC_VER) && defined(_DEBUG) && defined(ENABLE_LEAK_FINDER)
	PlgMgr->BindConsoleCommand("dumpmem", nullptr, " - Dumps all used memory blocks together with their callstacks into memdump.xml");
	#endif
}





void cServer::Shutdown(void)
{
	m_ListenThreadIPv4.Stop();
	m_ListenThreadIPv6.Stop();
	
	m_bRestarting = true;
	m_RestartEvent.Wait();

	cRoot::Get()->SaveAllChunks();

	cCSLock Lock(m_CSClients);
	for (ClientList::iterator itr = m_Clients.begin(); itr != m_Clients.end(); ++itr)
	{
		(*itr)->Destroy();
		delete *itr;
	}
	m_Clients.clear();
}





void cServer::KickUser(int a_ClientID, const AString & a_Reason)
{
	cCSLock Lock(m_CSClients);
	for (ClientList::iterator itr = m_Clients.begin(); itr != m_Clients.end(); ++itr)
	{
		if ((*itr)->GetUniqueID() == a_ClientID)
		{
			(*itr)->Kick(a_Reason);
		}
	}  // for itr - m_Clients[]
}





void cServer::AuthenticateUser(int a_ClientID, const AString & a_Name, const AString & a_UUID, const Json::Value & a_Properties)
{
	cCSLock Lock(m_CSClients);
	for (ClientList::iterator itr = m_Clients.begin(); itr != m_Clients.end(); ++itr)
	{
		if ((*itr)->GetUniqueID() == a_ClientID)
		{
			(*itr)->Authenticate(a_Name, a_UUID, a_Properties);
			return;
		}
	}  // for itr - m_Clients[]
}





////////////////////////////////////////////////////////////////////////////////
// cServer::cNotifyWriteThread:

cServer::cNotifyWriteThread::cNotifyWriteThread(void) :
	super("ClientPacketThread"),
	m_Server(nullptr)
{
}





cServer::cNotifyWriteThread::~cNotifyWriteThread()
{
	m_ShouldTerminate = true;
	m_Event.Set();
	Wait();
}





bool cServer::cNotifyWriteThread::Start(cServer * a_Server)
{
	m_Server = a_Server;
	return super::Start();
}





void cServer::cNotifyWriteThread::Execute(void)
{
	cClientHandleList Clients;
	while (!m_ShouldTerminate)
	{
		cCSLock Lock(m_CS);
		while (m_Clients.size() == 0)
		{
			cCSUnlock Unlock(Lock);
			m_Event.Wait();
			if (m_ShouldTerminate)
			{
				return;
			}
		}
		
		// Copy the clients to notify and unlock the CS:
		Clients.splice(Clients.begin(), m_Clients);
		Lock.Unlock();
		
		for (cClientHandleList::iterator itr = Clients.begin(); itr != Clients.end(); ++itr)
		{
			m_Server->m_SocketThreads.NotifyWrite(*itr);
		}  // for itr - Clients[]
		Clients.clear();
	}  // while (!mShouldTerminate)
}





void cServer::cNotifyWriteThread::NotifyClientWrite(const cClientHandle * a_Client)
{
	{
		cCSLock Lock(m_CS);
		m_Clients.remove(const_cast<cClientHandle *>(a_Client));  // Put it there only once
		m_Clients.push_back(const_cast<cClientHandle *>(a_Client));
	}
	m_Event.Set();
}




