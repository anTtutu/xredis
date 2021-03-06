#include "xRedis.h"

xRedis::xRedis(const char * ip, int16_t port, int16_t threadCount,bool enbaledCluster,bool enabledSentinel)
:host(ip),
port(port),
threadCount(threadCount),
masterPort(0),
clusterEnabled(enbaledCluster),
slaveEnabled(false),
authEnabled(false),
repliEnabled(false),
salveCount(0),
clusterSlotEnabled(false),
clusterRepliMigratEnabled(false),
clusterRepliImportEnabeld(false),
count(0),
pingPong(false)
{
	initConifg();
	createSharedObjects();
	loadDataFromDisk();
	server.init(&loop, host, port,this);
	server.setConnectionCallback(std::bind(&xRedis::connCallBack, this, std::placeholders::_1,std::placeholders::_2));
	server.setThreadNum(threadCount);
	server.start();
	zmalloc_enable_thread_safeness();
}

xRedis::~xRedis()
{
	destorySharedObjects();
	clearCommand();
}




bool xRedis::clearClusterMigradeCommand(void * data)
{

}

void xRedis::replyCheck()
{

}

void xRedis::test(void * data)
{
	count++;
	LOG_INFO << count;
}

void xRedis::handleTimeOut(void * data)
{
	loop.quit();
}

void xRedis::handleSetExpire(void * data)
{
	rObj * obj = (rObj*) data;
	int count = 0 ;
	removeCommand(obj,count);
}


void xRedis::handleSalveRepliTimeOut(void * data)
{
	int32_t *sockfd = (int32_t *)data;
	MutexLockGuard mu(slaveMutex);
	auto it = salvetcpconnMaps.find(*sockfd);
	if(it != salvetcpconnMaps.end())
	{
		it->second->forceClose();
	}
	LOG_INFO<<"sync connect repli  timeout ";
}



void xRedis::clearClusterState(int32_t sockfd)
{
	
}


void xRedis::clearRepliState(int32_t sockfd)
{
	{
		MutexLockGuard mu(slaveMutex);
		auto it = salvetcpconnMaps.find(sockfd);
		if(it != salvetcpconnMaps.end())
		{
			salveCount--;
			salvetcpconnMaps.erase(sockfd);
			if(salvetcpconnMaps.size() == 0)
			{
				repliEnabled = false;
				xBuffer buffer;
				slaveCached.swap(buffer);
			}
		}

		auto iter = repliTimers.find(sockfd);
		if(iter != repliTimers.end())
		{
			loop.cancelAfter(iter->second);
			repliTimers.erase(iter);
		}

	}


}
void xRedis::connCallBack(const xTcpconnectionPtr& conn,void *data)
{
	if(conn->connected())
	{
		//socket.setTcpNoDelay(conn->getSockfd(),true);
		socket.getpeerName(conn->getSockfd(),&(conn->host),conn->port);
		std::shared_ptr<xSession> session (new xSession(this,conn));
		MutexLockGuard mu(mutex);
		sessions[conn->getSockfd()] = session;
		//LOG_INFO<<"Client connect success";
	}
	else
	{
		{
			clearRepliState(conn->getSockfd());
		}

		{
			clearClusterState(conn->getSockfd());
		}
	
		{
			MutexLockGuard mu(mutex);
			sessions.erase(conn->getSockfd());
		}

		//LOG_INFO<<"Client disconnect";
	}
}


bool xRedis::deCodePacket(const xTcpconnectionPtr& conn,xBuffer *recvBuf,void  *data)
{
 	return true;
}


void xRedis::run()
{
	loop.run();
}


void xRedis::loadDataFromDisk()
{
	char rdb_filename[] = "dump.rdb";
	  
	if(rdbLoad(rdb_filename,this) == REDIS_OK)
	{
		LOG_INFO<<"load rdb success";
	}
	else if (errno != ENOENT)
	{
        	LOG_WARN<<"Fatal error loading the DB:  Exiting."<<strerror(errno);
 	}

}


bool xRedis::zrevrangeCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size()  < 3  || obj.size() > 4)
	{
		addReplyErrorFormat(session->sendBuf,"unknown zrevrange  error");
		return false;
	}

	obj[0]->calHash();
	size_t hash = obj[0]->hash;
	long long  begin = 0;
	string2ll(obj[1]->ptr,sdsllen(obj[1]->ptr),&begin);
	long long  end = 0;
	string2ll(obj[2]->ptr,sdsllen(obj[2]->ptr),&end);

	MutexLock &mu = sortSetShards[hash% kShards].mutex;
	auto &sset = sortSetShards[hash% kShards].sset;
	{
		MutexLockGuard lock(mu);
		auto it = sset.find(obj[0]);
		if(it != sset.end())
		{
			addReplyMultiBulkLen(session->sendBuf,sset.size() * 2);
			int i  = 0 ;
			for(auto iter = it->second.rbegin(); iter != it->second.rend(); iter ++)
			{
				addReplyBulkCBuffer(session->sendBuf,(*iter).value->ptr,sdsllen((*iter).value->ptr));
				addReplyBulkCBuffer(session->sendBuf,(*iter).key->ptr,sdsllen((*iter).key->ptr));
			}
		}
		else
		{
			addReplyMultiBulkLen(session->sendBuf,0);
		}
	}

	for(auto it  = obj.begin(); it != obj.end(); it ++)
	{
		zfree(*it);
	}

	return true;
}


bool xRedis::zaddCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() < 3  ||   (obj.size() - 1 ) % 2  != 0)
	{
		addReplyErrorFormat(session->sendBuf,"unknown zadd  error");
		return false;
	}

	int count = 0;
	obj[0]->calHash();
	size_t hash = obj[0]->hash;
	MutexLock &mu = sortSetShards[hash% kShards].mutex;
	auto &set = sortSetShards[hash% kShards].set;
	auto &sset = sortSetShards[hash% kShards].sset;
	{
		MutexLockGuard lock(mu);
		auto it = set.find(obj[0]);
		if(it == set.end())
		{
			SetMap setMap;
			std::set<rSObj> sObj;
		
			for(int i = 1; i < obj.size(); i +=2)
			{
				obj[i]->calHash();
				obj[i + 1]->calHash();
				auto iter = setMap.find(obj[i + 1]);
				if(iter == setMap.end())
				{
					rSObj robj;
					robj.key = obj[i];
					robj.value = obj[i + 1];
					sObj.insert(robj);
					setMap.insert(std::make_pair(obj[i + 1],obj[i]));
					count++;
				}
				else
				{
					{
						rSObj  robj;
						robj.key = iter->second;
						robj.value = iter->first;
						sObj.erase(robj);
					}

					zfree(obj [i + 1]);
					zfree(iter->second);
					iter->second = obj[i];
					
					{
						rSObj  robj;
						robj.key = iter->second;
						robj.value = iter->first;
						sObj.insert(robj);
					}
					
				}
			}
			
			set.insert(std::make_pair(obj[0],setMap));
			sset.insert(std::make_pair(obj[0],sObj));
			
		}
		else
		{
			auto itt = sset.find(obj[0]);
			if(itt == sset.end())
			{
				assert(false);
			}
			
			zfree(obj[0]);
			for(int i = 1; i < obj.size(); i +=2)
			{
				obj[i]->calHash();
				obj[i + 1]->calHash();
				auto iter = it->second.find(obj[i + 1]);
				if(iter == it->second.end())
				{
					rSObj robj;
					robj.key = obj[i];
					robj.value = obj[i + 1];
					itt->second.insert(robj);
					it->second.insert(std::make_pair(obj[i + 1],obj[i]));
					count++;
				}
				else
				{
					{
						rSObj  robj;
						robj.key = iter->second;
						robj.value = iter->first;
						itt->second.erase(robj);
					}
				
					zfree(obj [i + 1]);	
					zfree(iter->second);
					iter->second = obj[i];
					{
					
						rSObj  robj;
						robj.key = iter->second;
						robj.value = iter->first;
						itt->second.insert(robj);
					}
				
				}
			}
				
		}
	}

	addReplyLongLong(session->sendBuf,count);
	return true;
}

bool xRedis::zcardCommand(const std::deque <rObj*> & obj,xSession * session)
{
	return false;
}

bool xRedis::zcountCommand(const std::deque <rObj*> & obj,xSession * session)
{
	return false;
}


bool xRedis::keysCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 1 )
	{
		addReplyErrorFormat(session->sendBuf,"unknown keys  error");
		return false;
	}

	std::string des  = "*";
	std::string src = obj[0]->ptr;


	addReplyMultiBulkLen(session->sendBuf,getDbsize());

	{
		for(auto it = setMapShards.begin(); it != setMapShards.end(); it++)
		{
			auto &map = (*it).setMap;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter != map.end(); iter++)
			{
				addReplyBulkCBuffer(session->sendBuf,iter->first->ptr,sdsllen(iter->first->ptr));
			}

		}

	}

	{
		for(auto it = hsetMapShards.begin(); it != hsetMapShards.end(); it++)
		{
			auto &map = (*it).hsetMap;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter!=map.end(); iter++)
			{
				addReplyBulkCBuffer(session->sendBuf,iter->first->ptr,sdsllen(iter->first->ptr));
			}

		}

	}

	{
		for(auto it = setShards.begin(); it != setShards.end(); it++)
		{
			auto &map = (*it).set;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter != map.end(); iter ++)
			{
				addReplyBulkCBuffer(session->sendBuf,iter->first->ptr,sdsllen(iter->first->ptr));
			}

		}
	}



	{
		for(auto it = sortSetShards.begin(); it != sortSetShards.end(); it++)
		{
			auto &map = (*it).set;
			auto &mmap = (*it).sset;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter != map.end(); iter ++)
			{
				addReplyBulkCBuffer(session->sendBuf,iter->first->ptr,sdsllen(iter->first->ptr));
			}

		}
	}


	{
		for(auto it = pubSubShards.begin(); it != pubSubShards.end(); it++)
		{
			auto &map = (*it).pubSub;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter != map.end(); iter ++)
			{
				addReplyBulkCBuffer(session->sendBuf,iter->first->ptr,sdsllen(iter->first->ptr));
			}

		}
	}




	return false;
}

bool xRedis::zrangeCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size()  < 3  || obj.size() > 4)
	{
		addReplyErrorFormat(session->sendBuf,"unknown zrange  error");
		return false;
	}

	obj[0]->calHash();
	size_t hash = obj[0]->hash;
	long long  begin = 0;
	string2ll(obj[1]->ptr,sdsllen(obj[1]->ptr),&begin);
	long long  end = 0;
	string2ll(obj[2]->ptr,sdsllen(obj[2]->ptr),&end);
	MutexLock &mu =  sortSetShards[hash% kShards].mutex;
	auto &sset = sortSetShards[hash% kShards].sset; 
	{
		MutexLockGuard lock(mu);
		auto it = sset.find(obj[0]);
		if(it != sset.end())
		{
			addReplyMultiBulkLen(session->sendBuf,it->second.size() * 2);
			for(auto iter = it->second.begin(); iter != it->second.end(); iter ++)
			{
				addReplyBulkCBuffer(session->sendBuf,(*iter).value->ptr,sdsllen((*iter).value->ptr));
				addReplyBulkCBuffer(session->sendBuf,(*iter).key->ptr,sdsllen((*iter).key->ptr));
			}
		}
		else
		{
			addReplyMultiBulkLen(session->sendBuf,0);
		}
	
	}

	return false;
}

bool xRedis::zrankCommand(const std::deque <rObj*> & obj,xSession * session)
{
	return false;
}


bool xRedis::unsubscribeCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown unsubscribe  error");
		return false;
	}

	obj[0]->calHash();
	size_t hash = obj[0]->hash;
	MutexLock &mu = pubSubShards[hash% kShards].mutex;
	auto &pubSub = pubSubShards[hash% kShards].pubSub;
	{
		MutexLockGuard lock(mu);
		auto iter = pubSub.find(obj[0]);
		if(iter == pubSub.end())
		{
			addReply(session->sendBuf,shared.czero);
		}
		else
		{
			pubSub.erase(iter);
			zfree(iter->first);
			addReply(session->sendBuf,shared.ok);
		}
	}

	for(auto it = obj.begin(); it != obj.end(); it ++)
	{
		zfree(*it);
	}

	return true;
}


bool xRedis::sentinelCommand(const std::deque<rObj*> & obj, xSession * session)
{
	return false;
}

bool xRedis::memoryCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size()  > 2)
	{
		addReplyErrorFormat(session->sendBuf,"unknown memory  error");
		return false;
		}
#ifdef USE_JEMALLOC

		char tmp[32];
		unsigned narenas = 0;
		size_t sz = sizeof(unsigned);
		if (!je_mallctl("arenas.narenas", &narenas, &sz, NULL, 0))
		{
		sprintf(tmp, "arena.%d.purge", narenas);
		if (!je_mallctl(tmp, NULL, 0, NULL, 0))
		{
			addReply(session->sendBuf, shared.ok);
			return false;
		}
	}

#endif

	addReply(session->sendBuf,shared.ok);


	return false;
}

bool xRedis::infoCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size()  < 0)
	{
		addReplyErrorFormat(session->sendBuf,"unknown info  error");
		return false;
	}

	struct rusage self_ru, c_ru;
	getrusage(RUSAGE_SELF, &self_ru);
	getrusage(RUSAGE_CHILDREN, &c_ru);
	sds info = sdsempty();

	char hmem[64];
	char peak_hmem[64];
	char total_system_hmem[64];
	char used_memory_rss_hmem[64];
	char maxmemory_hmem[64];
	size_t zmalloc_used = zmalloc_used_memory();
	size_t total_system_mem =  zmalloc_get_memory_size();
	bytesToHuman(hmem,zmalloc_used);
	bytesToHuman(peak_hmem,zmalloc_used);
	bytesToHuman(total_system_hmem,total_system_mem);
	bytesToHuman(used_memory_rss_hmem,zmalloc_get_rss());
	bytesToHuman(maxmemory_hmem, 8);

	info = sdscatprintf(info,
	"# Memory\r\n"
	"used_memory:%zu\r\n"
	"used_memory_human:%s\r\n"
	"used_memory_rss:%zu\r\n"
	"used_memory_rss_human:%s\r\n"
	"used_memory_peak:%zu\r\n"
	"used_memory_peak_human:%s\r\n"
	"total_system_memory:%lu\r\n"
	"total_system_memory_human:%s\r\n"
	"maxmemory_human:%s\r\n"
	"mem_fragmentation_ratio:%.2f\r\n"
	"mem_allocator:%s\r\n",
	zmalloc_used,
	hmem,
	zmalloc_get_rss(),
	used_memory_rss_hmem,
	zmalloc_used,
	peak_hmem,
	(unsigned long)total_system_mem,
	total_system_hmem,
	maxmemory_hmem,
	zmalloc_get_fragmentation_ratio(zmalloc_get_rss()),
	ZMALLOC_LIB
	);

	info = sdscat(info,"\r\n");
	info = sdscatprintf(info,
	"# CPU\r\n"
	"used_cpu_sys:%.2f\r\n"
	"used_cpu_user:%.2f\r\n"
	"used_cpu_sys_children:%.2f\r\n"
	"used_cpu_user_children:%.2f\r\n",
	(float)self_ru.ru_stime.tv_sec+(float)self_ru.ru_stime.tv_usec/1000000,
	(float)self_ru.ru_utime.tv_sec+(float)self_ru.ru_utime.tv_usec/1000000,
	(float)c_ru.ru_stime.tv_sec+(float)c_ru.ru_stime.tv_usec/1000000,
	(float)c_ru.ru_utime.tv_sec+(float)c_ru.ru_utime.tv_usec/1000000);

	info = sdscat(info,"\r\n");
	info = sdscatprintf(info,
	"# Server\r\n"
	"tcp_connect_count:%d\r\n"
	"local_ip:%s\r\n"
	"local_port:%d\r\n"
	"local_thread_count:%d\n",
	sessions.size(),
	host.c_str(),
	port,
	threadCount);

	{
		MutexLockGuard lock(slaveMutex);
		for(auto it = salvetcpconnMaps.begin(); it != salvetcpconnMaps.end(); it ++)
		{
			info = sdscat(info,"\r\n");
			info = sdscatprintf(info,
			"# SlaveInfo \r\n"
			"slave_ip:%s\r\n"
			"slave_port:%d\r\n",
			it->second->host.c_str(),
			it->second->port);

		}

	}
	
	addReplyBulkSds(session->sendBuf, info);
	
	return false ;
}


bool xRedis::clientCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() > 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown client  error");
		return false;
	}

	addReply(session->sendBuf,shared.ok);

	return false;
}


bool xRedis::echoCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() > 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown echo  error");
		return false;
	}

	addReplyBulk(session->sendBuf,obj[0]);
	return false;
}

bool xRedis::publishCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size()  != 2)
	{
		addReplyErrorFormat(session->sendBuf,"unknown publish  error");
		return false;
	}

	obj[0]->calHash();
	size_t hash = obj[0]->hash;
	MutexLock &mu = pubSubShards[hash% kShards].mutex;
	auto &pubSub = pubSubShards[hash% kShards].pubSub;
	{
		MutexLockGuard lock(mu);
		auto iter = pubSub.find(obj[0]);
		if(iter != pubSub.end())
		{
			for(auto list = iter->second.begin(); list != iter->second.end(); list ++)
			{
				session->pubSubTcpconn.push_back(*list);
			}

			addReply(session->sendPubSub,shared.mbulkhdr[3]);
			addReply(session->sendPubSub,shared.messagebulk);
			addReplyBulk(session->sendPubSub,obj[0]);
			addReplyBulk(session->sendPubSub,obj[1]);
			addReplyLongLong(session->sendPubSub,iter->second.size());
			addReply(session->sendBuf,shared.ok);
		}
		else
		{
			addReply(session->sendBuf,shared.czero);
		}
	}

	return false;
}

bool xRedis::subscribeCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size()  < 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown subscribe  error");
		return false;
	}
	int count = 0;

	for(auto it = obj.begin(); it != obj.end(); it++)
	{
	     (*it)->calHash();
	     size_t hash = (*it)->hash;

	     MutexLock &mu = pubSubShards[hash% kShards].mutex;
		auto &pubSub = pubSubShards[hash% kShards].pubSub;
		{
			MutexLockGuard lock(mu);
			auto iter = pubSub.find(*it);
			if(iter != pubSub.end())
			{
				zfree(*it);
				iter->second.push_back(session->conn);
			}
			else
			{
				std::list<xTcpconnectionPtr> list;
				list.push_back(session->conn);
				pubSub.insert(std::make_pair(*it, std::move(list)));
			}

		}

		addReply(session->sendBuf, shared.mbulkhdr[3]);
		addReply(session->sendBuf, shared.subscribebulk);
		addReplyBulk(session->sendBuf, *it);
		addReplyLongLong(session->sendBuf, ++count);

	}

	return true;
}

bool xRedis::authCommand(const std::deque <rObj*> & obj, xSession * session)
{
	if (obj.size() > 1)
	{
		addReplyErrorFormat(session->sendBuf, "unknown auth  error");
		return false;
	}

	if (password.c_str() == nullptr)
	{
		addReplyError(session->sendBuf, "Client sent AUTH, but no password is set");
		return false;
	}

	if (!strcasecmp(obj[0]->ptr, password.c_str()))
	{
		session->authEnabled = true;
		addReply(session->sendBuf, shared.ok);
	}
	else
	{
		addReplyError(session->sendBuf, "invalid password");
	}


	return false;
}

bool xRedis::configCommand(const std::deque <rObj*> & obj, xSession * session)
{

	if (obj.size() > 3)
	{
		addReplyErrorFormat(session->sendBuf, "unknown config  error");
		return false;
	}

	if (!strcasecmp(obj[0]->ptr, "set"))
	{
		if (obj.size() != 3)
		{
			addReplyErrorFormat(session->sendBuf, "Wrong number of arguments for CONFIG %s",
				(char*)obj[0]->ptr);
			return false;
		}

		if (!strcasecmp(obj[1]->ptr, "requirepass"))
		{
			password = obj[2]->ptr;
			authEnabled = true;
			session->authEnabled = false;
			addReply(session->sendBuf, shared.ok);
		}
		else
		{
			addReplyErrorFormat(session->sendBuf, "Invalid argument  for CONFIG SET '%s'",
				(char*)obj[1]->ptr);
		}

	}
	else
	{
		addReplyError(session->sendBuf, "CONFIG subcommand must be one of GET, SET, RESETSTAT, REWRITE");
	}

	return false;

}

bool xRedis::migrateCommand(const std::deque<rObj*> & obj, xSession * session)
{
	if (obj.size() < 2)
	{
		addReplyErrorFormat(session->sendBuf, "unknown migrate  error");
	}

	if (!clusterEnabled)
	{
		addReplyError(session->sendBuf, "This instance has cluster support disabled");
		return false;
	}

	long long port;

	if (getLongLongFromObject(obj[1], &port) != REDIS_OK)
	{
		addReplyErrorFormat(session->sendBuf, "Invalid TCP port specified: %s",
			(char*)obj[2]->ptr);
		return false;
	}


	std::string ip = obj[0]->ptr;
	if (ip == host  &&  this->port == port)
	{
		addReplyErrorFormat(session->sendBuf, "migrate  self server error ");
		return false;
	}

	clus.asyncReplicationToNode( ip , port);

	addReply(session->sendBuf, shared.ok);
	return false;
}

bool xRedis::clusterCommand(const std::deque <rObj*> & obj, xSession * session)
{
	if (!clusterEnabled)
	{
		addReplyError(session->sendBuf, "This instance has cluster support disabled");
		return false;
	}

	if (!strcasecmp(obj[0]->ptr, "meet"))
	{
		if (obj.size() != 3)
		{
			addReplyErrorFormat(session->sendBuf, "unknown cluster  error");
			return false;
		}

		long long port;

		if (getLongLongFromObject(obj[2], &port) != REDIS_OK)
		{
			addReplyErrorFormat(session->sendBuf, "Invalid TCP port specified: %s",
				(char*)obj[2]->ptr);
			return false;
		}

		if (host.c_str() && !memcmp(host.c_str(), obj[1]->ptr, sdsllen(obj[1]->ptr))
			&& this->port == port)
		{
			LOG_WARN << "cluster  meet  connect self error .";
			addReplyErrorFormat(session->sendBuf, "Don't connect self ");
			return false;
		}

		{
			MutexLockGuard lk(clusterMutex);
			for (auto it = clustertcpconnMaps.begin(); it != clustertcpconnMaps.end(); it++)
			{
				if (port == it->second->port && !memcmp(it->second->host.c_str(), obj[1]->ptr, sdsllen(obj[1]->ptr)))
				{
					LOG_WARN << "cluster  meet  already exists .";
					addReplyErrorFormat(session->sendBuf, "cluster  meet  already exists ");
					return false;
				}
			}
		}
		clus.connSetCluster(obj[1]->ptr, port, this);
	}
	else if (!strcasecmp(obj[0]->ptr, "connect") && obj.size() == 3)
	{
		long long  port;
	
		if (getLongLongFromObject(obj[2], &port) != REDIS_OK)
		{
			addReplyError(session->sendBuf, "Invalid or out of range port");
			return  false;
		}

		{
			MutexLockGuard lk(clusterMutex);
			for (auto it = clustertcpconnMaps.begin(); it != clustertcpconnMaps.end(); it++)
			{
				if (port == it->second->port && !memcmp(it->second->host.c_str(), obj[1]->ptr, sdsllen(obj[1]->ptr)))
				{
					return false;
				}
			}
		}
	
		clus.connSetCluster(obj[1]->ptr, (int32_t)port, this);
		return false;
	}
	else if (!strcasecmp(obj[0]->ptr, "info") && obj.size() == 1)
	{
		rObj *o;
		MutexLockGuard lk(clusterMutex);
		sds ci = sdsempty(), ni = sdsempty();

		ci = sdscatprintf(sdsempty(), "%s %s:%d ------connetc slot:",
			(host + "::" + std::to_string(port)).c_str(),
			host.c_str(),
			port);

		for (auto it = clus.clusterSlotNodes.begin(); it != clus.clusterSlotNodes.end(); it++)
		{
			if (it->second.ip == host && it->second.port == port)
			{
				ni = sdscatprintf(sdsempty(), "%d ",
					it->first);
				ci = sdscatsds(ci, ni);
				sdsfree(ni);
			}
		}
		ci = sdscatlen(ci, "\n", 1);

		for (auto it = clustertcpconnMaps.begin(); it != clustertcpconnMaps.end(); it++)
		{
			ni = sdscatprintf(sdsempty(), "%s %s:%d ------connetc slot:",
				(it->second->host +  "::" + std::to_string(it->second->port)).c_str(),
				it->second->host.c_str(),
				it->second->port);

			ci = sdscatsds(ci, ni);
			sdsfree(ni);
		
			for (auto iter = clus.clusterSlotNodes.begin(); iter != clus.clusterSlotNodes.end(); iter++)
			{
				if (iter->second.ip == it->second->host && iter->second.port == it->second->port)
				{
					ni = sdscatprintf(sdsempty(), "%d ",
						iter->first);
					ci = sdscatsds(ci, ni);
					sdsfree(ni);
				}
				
			}
			ci = sdscatlen(ci, "\n", 1);

		}

		o = createObject(OBJ_STRING, ci);
		addReplyBulk(session->sendBuf, o);
		decrRefCount(o);
		return false;
	}
	else if (!strcasecmp(obj[0]->ptr, "slots") && obj.size() == 1)
	{

	}
	else if (!strcasecmp(obj[0]->ptr, "keyslot") && obj.size() == 2)
	{
		const char * key = obj[1]->ptr;
		addReplyLongLong(session->sendBuf, clus.keyHashSlot((char*)key, sdsllen(key)));
		return false;
	}
	else if (!strcasecmp(obj[0]->ptr, "setslot") && obj.size() >= 4)
	{
		int slot;
		if ((slot = clus.getSlotOrReply(session, obj[1])) == -1)
		{
			addReplyErrorFormat(session->sendBuf, "Invalid slot %d",
				(char*)obj[1]->ptr);
			return false;
		}

		
		if( !strcasecmp(obj[2]->ptr, "node") )
		{
			MutexLockGuard lk(clusterMutex);
			std::string ipPort = obj[3]->ptr;
			std::string imipPort = obj[4]->ptr;
			const char *start = obj[3]->ptr;
			const char *end = obj[3]->ptr + sdsllen(obj[3]->ptr );
			const  char *space = std::find(start,end,':');
			if(space != end)
			{
				std::string ip(start,space);
				std::string port(space + 2,end);
				long long value;
				string2ll(port.c_str(), port.length(), &value);
				auto it = clus.clusterSlotNodes.find(slot);
				if(it != clus.clusterSlotNodes.end())
				{
					it->second.ip = ip;
					it->second.port = value;
				}

				
				clus.importingSlotsFrom.erase(imipPort);

				if(clus.importingSlotsFrom.size() == 0)
				{
					clusterRepliImportEnabeld = false;
				}
		
			}
		
			LOG_INFO<<"importingSlotsFrom  erase ";
			//addReply(session->sendBuf, shared.ok);
			return false;
		}


		std::string nodeName = obj[3]->ptr;
		if (nodeName == host + "::" + std::to_string(port))
		{
			addReplyErrorFormat(session->sendBuf, "setslot self server error ");
			return false;
		}


		if( !strcasecmp(obj[2]->ptr, "importing") && obj.size() == 4)
		{
			bool mark = false;
			{
				MutexLockGuard lk(clusterMutex);
				for (auto it = clus.clusterSlotNodes.begin(); it != clus.clusterSlotNodes.end(); it++)
				{
					std::string node = it->second.ip + "::" + std::to_string(it->second.port);
					if (node == nodeName && slot == it->first)
					{
						mark = true;
						break;
						
					}
				}
			}

			if (!mark)
			{
				addReplyErrorFormat(session->sendBuf, "setslot slot node no found error ");
				return false;
			}
		
			MutexLockGuard lk(clusterMutex);
			auto it = clus.importingSlotsFrom.find(nodeName);
			if (it == clus.importingSlotsFrom.end())
			{
				std::unordered_set<int32_t> uset;
				uset.insert(slot);
				clus.importingSlotsFrom.insert(std::make_pair(std::move(nodeName), std::move(uset)));
			}
			else
			{
				auto iter = it->second.find(slot);
				if (iter == it->second.end())
				{
					it->second.insert(slot);
				}
				else
				{
					addReplyErrorFormat(session->sendBuf, "repeat importing slot :%d", slot);
					return false;
				}

			}

			clusterRepliImportEnabeld = true;
		}
		else if (!strcasecmp(obj[2]->ptr, "migrating") && obj.size() == 4)
		{
			MutexLockGuard lk(clusterMutex);
			auto it = clus.migratingSlosTos.find(nodeName);
			if (it == clus.migratingSlosTos.end())
			{
				std::unordered_set<int32_t> uset;
				uset.insert(slot);
				clus.migratingSlosTos.insert(std::make_pair(std::move(nodeName), std::move(uset)));
			}
			else
			{
				auto iter = it->second.find(slot);
				if (iter == it->second.end())
				{
					it->second.insert(slot);
				}
				else
				{
					addReplyErrorFormat(session->sendBuf, "repeat migrating slot :%d", slot);
					return false;
				}
			}
		}
		else
		{
			addReplyErrorFormat(session->sendBuf, "Invalid  param ");
			return false;
		}

		
	}
	else if(!strcasecmp(obj[0]->ptr, "delimport"))
	{
		MutexLockGuard lk(clusterMutex);
		clus.importingSlotsFrom.clear();
		clusterRepliImportEnabeld = false;
		LOG_INFO << "delimport success:";
		return false;
	}
	else if (!strcasecmp(obj[0]->ptr, "delsync"))
	{
		int slot;
		if ((slot = clus.getSlotOrReply(session, obj[1])) == 0)
		{
			LOG_INFO << "getSlotOrReply error ";
			return false;
		}

		MutexLockGuard lk(clusterMutex);
		clus.clusterSlotNodes.erase(slot);
		LOG_INFO << "delsync success:" << slot;
		return false;

	}
	else if (!strcasecmp(obj[0]->ptr, "addsync"))
	{
		int slot;
		long long  p;
		if ((slot = clus.getSlotOrReply(session, obj[1])) == 0)
		{
			LOG_INFO << "getSlotOrReply error ";
			return false;
		}

		if (getLongLongFromObject(obj[3], &p) != REDIS_OK)
		{
			addReplyError(session->sendBuf, "Invalid or out of range port");
			return  REDIS_ERR;
		}

		MutexLockGuard lk(clusterMutex);
		auto it = clus.clusterSlotNodes.find(slot);
		if (it == clus.clusterSlotNodes.end())
		{
			clusterSlotEnabled = true;
			LOG_INFO << "addsync success:" << slot;
			xClusterNode node;
			node.ip = obj[2]->ptr;
			node.port = (int32_t)p;
			clus.clusterSlotNodes.insert(std::make_pair(slot, std::move(node)));

		}
		else
		{
			LOG_INFO << "clusterSlotNodes insert error ";
		}

		return false;
		
	}
	else if (!strcasecmp(obj[0]->ptr, "delslots") &&  obj.size() == 2)
	{		
		MutexLockGuard lk(clusterMutex);
		if (clustertcpconnMaps.size() == 0)
		{
			addReplyErrorFormat(session->sendBuf, "execute cluster meet ip:port");
			return false;
		}

		int slot;
		int j;
		for (j = 1; j < obj.size(); j++)
		{
			if ((slot = clus.getSlotOrReply(session, obj[j])) == 0)
			{
				return false;
			}

			if(slot < 0 || slot > 16384 )
			{
				addReplyErrorFormat(session->sendBuf, "cluster delslots range error %d:",slot);
				return false;
			}

			auto it = clus.clusterSlotNodes.find(slot);
			if (it != clus.clusterSlotNodes.end())
			{
				std::deque<rObj*> robj;
				rObj * c = createStringObject("cluster", 7);
				rObj * d = createStringObject("delsync", 7);
				robj.push_back(c);
				robj.push_back(d);
				rObj * o = createStringObject(obj[j]->ptr, sdsllen(obj[j]->ptr));
				robj.push_back(o);
				clus.syncClusterSlot(robj);
				clus.clusterSlotNodes.erase(slot);
				LOG_INFO << "deslots success " << slot;
			}
		}

		if (clus.clusterSlotNodes.size() == 0)
		{
			clusterSlotEnabled = false;
		}

	}
	else if (!strcasecmp(obj[0]->ptr, "addslots") && obj.size() == 2)
	{
		int32_t  j, slot;
		for (j = 1; j < obj.size(); j++)
		{
			if(slot < 0 || slot > 16384 )
			{
				addReplyErrorFormat(session->sendBuf, "cluster delslots range error %d:",slot);
				return false;
			}

			if ((slot = clus.getSlotOrReply(session, obj[j])) == 0)
			{
				return false;
			}

			MutexLockGuard lk(clusterMutex);
			if (clustertcpconnMaps.size() == 0)
			{
				addReplyErrorFormat(session->sendBuf, "execute cluster meet ip:port");
				return false;
			}

			auto it = clus.clusterSlotNodes.find(slot);
			if (it == clus.clusterSlotNodes.end())
			{
				xClusterNode  node;
				node.ip = host;
				node.port = this->port;
				rObj * i = createStringObject(host.c_str(), host.length());
				char buf[32];
				int32_t len = ll2string(buf, sizeof(buf), this->port);
				rObj * p = createStringObject((const char*)buf, len);

				std::deque<rObj*> robj;
				rObj * c = createStringObject("cluster", 7);
				rObj * a = createStringObject("addsync", 7);
			
				robj.push_back(c);
				robj.push_back(a);
				rObj * o = createStringObject(obj[j]->ptr, sdsllen(obj[j]->ptr));
				robj.push_back(o);
				robj.push_back(i);
				robj.push_back(p);

				clus.syncClusterSlot(robj);
				clus.clusterSlotNodes.insert(std::make_pair(slot, std::move(node)));
				LOG_INFO << "addslots success " << slot;
			}
			else
			{
				addReplyErrorFormat(session->sendBuf, "Slot %d specified multiple times", slot);
				return false;
			}
		}
		clusterSlotEnabled = true;

	}
	else
	{
		addReplyErrorFormat(session->sendBuf, "unknown param error");
		return false;
	}
	
	addReply(session->sendBuf, shared.ok);
	return false;
	
}

void xRedis::structureRedisProtocol(xBuffer &  sendBuf, std::deque<rObj*> &robjs)
{
	int len, j;
	char buf[32];
	buf[0] = '*';
	len = 1 + ll2string(buf + 1, sizeof(buf) - 1, robjs.size());
	buf[len++] = '\r';
	buf[len++] = '\n';
	sendBuf.append(buf, len);

	for (int i = 0; i < robjs.size(); i++)
	{
		buf[0] = '$';
		len = 1 + ll2string(buf + 1, sizeof(buf) - 1, sdsllen(robjs[i]->ptr));
		buf[len++] = '\r';
		buf[len++] = '\n';
		sendBuf.append(buf, len);
		sendBuf.append(robjs[i]->ptr, sdsllen(robjs[i]->ptr));
		sendBuf.append("\r\n", 2);
	}
}


bool  xRedis::save(xSession * session)
{
	int64_t start =  mstime();
	{
		MutexLockGuard lk(mutex);
		char filename[] = "dump.rdb";
		if(rdbSave(filename,this) == REDIS_OK)
		{
			LOG_INFO<<"Save rdb success";
		}
		else
		{
			LOG_INFO<<"Save rdb failure";

			return false;
		}
	}

	return true;
}


bool xRedis::bgsaveCommand(const std::deque <rObj*> & obj,xSession * session)
{
	
	if(obj.size() > 0)
	{
		addReplyErrorFormat(session->sendBuf,"unknown bgsave error");
		return false;
	}
	
	if(save(session))
	{
		addReply(session->sendBuf,shared.ok);
			
	}
	else
	{
		addReply(session->sendBuf,shared.err);
	}

	return true;
	
}

bool xRedis::saveCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() > 0)
	{
		addReplyErrorFormat(session->sendBuf,"unknown save error");
		return false;
	}

	if(save(session))
	{
		addReply(session->sendBuf,shared.ok);
	}
	else
	{
		addReply(session->sendBuf,shared.err);
	}

	return true;
}


bool xRedis::slaveofCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() !=  2)
	{
		addReplyErrorFormat(session->sendBuf,"unknown slaveof error");
		return false;
	}

	
	if (!strcasecmp(obj[0]->ptr,"no") &&!strcasecmp(obj[1]->ptr,"one")) 
	{
		if (masterHost.c_str() && masterPort) 
		{
			LOG_WARN<<"MASTER MODE enabled (user request from "<<masterHost.c_str()<<":"<<masterPort;
			repli.client->disconnect();
			repli.isreconnect = false;
		}

	}
	else
	{
		long   port;
		if ((getLongFromObjectOrReply(session->sendBuf, obj[1], &port, nullptr) != REDIS_OK))
			return false;

		if (host.c_str() && !memcmp(host.c_str(), obj[0]->ptr,sdsllen(obj[0]->ptr))
		&& this->port == port)
		{
			LOG_WARN<<"SLAVE OF connect self error .";
			addReplySds(session->sendBuf,sdsnew("Don't connect master self \r\n"));
			return false;
		}

		if (masterPort > 0)
		{
			LOG_WARN<<"SLAVE OF would result into synchronization with the master we are already connected with. No operation performed.";
			addReplySds(session->sendBuf,sdsnew("+OK Already connected to specified master\r\n"));
			return false;
		}	

		repli.replicationSetMaster(this,obj[0],port);
		LOG_INFO<<"SLAVE OF "<<obj[0]->ptr<<":"<<port<<" enabled (user request from client";
	}
	


	addReply(session->sendBuf,shared.ok);
	return false;
}



bool xRedis::commandCommand(const std::deque <rObj*> & obj,xSession * session)
{	
	addReply(session->sendBuf,shared.ok);
	return false;
}


bool xRedis::syncCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() >  0)
	{
		addReplyErrorFormat(session->sendBuf,"unknown sync  error");
		return false;
	}

	xTimer * timer = nullptr;
	{
		MutexLockGuard lk(slaveMutex);
		auto it = repliTimers.find(session->conn->getSockfd());
		if(it != repliTimers.end())
		{
			LOG_WARN<<"Client repeat send sync ";
			session->conn->forceClose();
			return false;
		}

		int32_t sockfd = session->conn->getSockfd();
		timer = session->conn->getLoop()->runAfter(REPLI_TIME_OUT,(void *)&sockfd,
				false,std::bind(&xRedis::handleSalveRepliTimeOut,this,std::placeholders::_1));
		repliTimers.insert(std::make_pair(session->conn->getSockfd(),timer));
		salvetcpconnMaps.insert(std::make_pair(session->conn->getSockfd(),session->conn));
		repliEnabled = true;
	}

	if(!save(session))
	{
		repli.isreconnect =false;
		session->conn->forceClose();
		return false;
	}


	bool mark  = true;
	char rdbFileName[] = "dump.rdb";
	{
		{
			MutexLockGuard lk(mutex);
			mark = rdbReplication(rdbFileName,session);
		}

		if(!mark)
		{
			MutexLockGuard lk(slaveMutex);
			if(timer)
			{
				session->conn->getLoop()->cancelAfter(timer);
			}
			session->conn->forceClose();
			LOG_INFO<<"master sync send failure";
			return false;
		}
	}

	LOG_INFO<<"master sync send success ";
	return true;
}


bool xRedis::psyncCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() >  0)
	{
		LOG_WARN<<"unknown psync  error";
		addReplyErrorFormat(session->sendBuf,"unknown psync  error");
		return false;
	}

	return true;
}


size_t xRedis::getDbsize()
{
	size_t size = 0;
	{

		for(auto it = setMapShards.begin(); it != setMapShards.end(); it++)
		{
			MutexLock &mu = (*it).mutex;
			MutexLockGuard lk(mu);
			size+=(*it).setMap.size();
		}
	}

	{
		for(auto it = hsetMapShards.begin(); it != hsetMapShards.end(); it++)
		{
			MutexLock &mu = (*it).mutex;
			MutexLockGuard lk(mu);
			size+=(*it).hsetMap.size();
		}
	}


	{
		for(auto it = setShards.begin(); it != setShards.end(); it++)
		{
			MutexLock &mu = (*it).mutex;
			MutexLockGuard lk(mu);
			size+=(*it).set.size();
		}
	}



	{
		for(auto it = sortSetShards.begin(); it != sortSetShards.end(); it++)
		{
			MutexLock &mu = (*it).mutex;
			MutexLockGuard lk(mu);
			size+=(*it).set.size();
		}
	}


	{
		for(auto it = pubSubShards.begin(); it != pubSubShards.end(); it++)
		{
			MutexLock &mu = (*it).mutex;
			MutexLockGuard lk(mu);
			size+=(*it).pubSub.size();
		}
	}

	return size;
}

bool xRedis::dbsizeCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() > 0)
	{
		addReplyErrorFormat(session->sendBuf,"unknown dbsize error");
		return false;
	}

	addReplyLongLong(session->sendBuf,getDbsize());

	return true;
}



int  xRedis::removeCommand(rObj * obj,int &count)
{
	
	{
		MutexLock &mu = setMapShards[obj->hash% kShards].mutex;
		auto &setMap = setMapShards[obj->hash% kShards].setMap;
		{
			MutexLockGuard lock(mu);
			auto it = setMap.find(obj);
			if(it != setMap.end())
			{
				auto iter = expireTimers.find(obj);
				if(iter != expireTimers.end())
				{
					expireTimers.erase(iter);
					loop.cancelAfter(iter->second);
				}

				count ++;
				setMap.erase(it);
				zfree(it->first);
				zfree(it->second);
			}

		}
	}

	{
		MutexLock &mu = hsetMapShards[obj->hash% kShards].mutex;
		auto &hsetMap = hsetMapShards[obj->hash% kShards].hsetMap;
		{
			MutexLockGuard lock(mu);
			auto hmap = hsetMap.find(obj);
			if(hmap != hsetMap.end())
			{
				count ++;
				for(auto iter = hmap->second.begin(); iter != hmap->second.end(); iter++)
				{
					zfree(iter->first);
					zfree(iter->second);
				}

				hsetMap.erase(hmap);
				zfree(hmap->first);
			}


		}
	}

	{
		MutexLock &mu = setShards[obj->hash% kShards].mutex;
		auto &set = setShards[obj->hash% kShards].set;
		{
			MutexLockGuard lock(mu);
			auto it = set.find(obj);
			if(it != set.end())
			{
				count ++;
				for(auto iter = it->second.begin(); iter != it->second.end(); iter++)
				{
					zfree(*iter);
				}

				set.erase(it);
				zfree(it->first);
			}
		}
	}



	{
		MutexLock &mu = sortSetShards[obj->hash% kShards].mutex;
		auto &set = sortSetShards[obj->hash% kShards].set;
		auto &sset = sortSetShards[obj->hash% kShards].sset;
		{
			MutexLockGuard lock(mu);
			auto it = set.find(obj);
			if(it != set.end())
			{
				auto itt = sset.find(obj);
				if(itt == sset.end())
				{
					assert(false);
				}
				
				count  ++;
				for(auto iter = it->second.begin(); iter != it->second.end(); iter++)
				{
					zfree(iter->first);
					zfree(iter->second);
				}

				set.erase(it);
				zfree(it->first);
				sset.erase(itt);

			}
		}
	}


	{
		MutexLock &mu = pubSubShards[obj->hash% kShards].mutex;
		auto &pubSub = pubSubShards[obj->hash% kShards].pubSub;
		{
			MutexLockGuard lock(mu);
			auto it = pubSub.find(obj);
			if(it != pubSub.end())
			{
				count  ++;
				pubSub.erase(it);
				zfree(it->first);
			}
		}
	}

	return  count;
}


bool xRedis::delCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  del error");
	}

	int count = 0;
	for(int i = 0 ; i < obj.size(); i ++)
	{
		obj[i]->calHash();		
		removeCommand(obj[i],count);
		zfree(obj[i]);
	}

	addReplyLongLong(session->sendBuf,count);
	
	return true;
}


bool setnxCommand(const std::deque <rObj*> & obj,xSession * session)
{
	return true;
}


bool xRedis::scardCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 1 )
	{
		addReplyErrorFormat(session->sendBuf,"unknown  scard error");
		return false;
	}

	obj[0]->calHash();
	size_t hash= obj[0]->hash;
	int count = 0;
	MutexLock &mu = setShards[hash% kShards].mutex;
	auto &set = setShards[hash% kShards].set;
	{
		MutexLockGuard lock(mu);
		auto it = set.find(obj[0]);
		if(it != set.end())
		{
			count = it->second.size();
		}
	}

	 addReplyLongLong(session->sendBuf,count);

	for(auto it = obj.begin(); it != obj.end(); it ++)
	{
		zfree(*it);
	}
	return true;
}

bool xRedis::saddCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() < 2)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  sadd error");
		return false;
	}

	obj[0]->calHash();
	size_t hash= obj[0]->hash;

	int count = 0;
	MutexLock &mu = setShards[hash% kShards].mutex;
	auto &set = setShards[hash% kShards].set;
	{
		MutexLockGuard lock(mu);
		auto it = set.find(obj[0]);
		if(it == set.end())
		{
			std::unordered_set<rObj*,Hash,Equal> uset;
			for(int i = 1; i < obj.size(); i ++)
			{
				obj[i]->calHash();
				auto iter = uset.find(obj[i]);
				if(iter == uset.end())
				{
					count++;
					uset.insert(obj[i]);
				}
				else
				{
					zfree(obj[i]);
				}
				set.insert(std::make_pair(obj[0],std::move(uset)));
			}
		}
		else
		{
			zfree(obj[0]);
			for(int i = 1; i < obj.size(); i ++)
			{
				obj[i]->calHash();
				auto iter = it->second.find(obj[i]);
				if(iter == it->second.end())
				{
					count++;
					it->second.insert(obj[i]);
				}
				else
				{
					zfree(obj[i]);
				}
			}
		}
	}

	addReplyLongLong(session->sendBuf,count);

	return true;
}

bool xRedis::selectCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  select error");
		return false;
	}

	for(auto it = obj.begin(); it != obj.end(); it ++)
	{
		zfree(*it);
	}


	addReply(session->sendBuf,shared.ok);
	return true;
}

bool xRedis::hkeysCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  hkeys error");
		return false;
	}

	obj[0]->calHash();
    size_t hash= obj[0]->hash;


    MutexLock &mu = hsetMapShards[hash% kShards].mutex;
	auto &hsetMap = hsetMapShards[hash% kShards].hsetMap;
	{
		MutexLockGuard lock(mu);

		auto it = hsetMap.find(obj[0]);
		if(it == hsetMap.end())
		{
			addReply(session->sendBuf,shared.emptymultibulk);
			return false;
		}

		addReplyMultiBulkLen(session->sendBuf,it->second.size());

		for(auto iter = it->second.begin(); iter != it->second.end(); iter++)
		{
			addReplyBulkCBuffer(session->sendBuf,iter->first->ptr,sdsllen(iter->first->ptr));
		}
	}

	return false;

}


bool xRedis::ppingCommand(const std::deque <rObj*> & obj, xSession * session)
{
	std::deque<rObj*>  robjs;
	robjs.push_back(shared.ppong);
	structureRedisProtocol(session->sendBuf,robjs);
	return true;
}

bool xRedis::ppongCommand(const std::deque <rObj*> & obj, xSession * session)
{
	pingPong = true;
	return true;
}


bool xRedis::pongCommand(const std::deque <rObj*> & obj,xSession * session)
{
	return false;
}

bool xRedis::pingCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() > 0)
	{
		addReplyErrorFormat(session->sendBuf,"unknown ping error");
		return false;
	}
	
	addReply(session->sendBuf,shared.pong);
	return true;
}


bool xRedis::hgetallCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  hgetall error");
		return false;
	}

	obj[0]->calHash();
	size_t hash= obj[0]->hash;
	
	MutexLock &mu = hsetMapShards[hash% kShards].mutex;
	auto &hsetMap = hsetMapShards[hash% kShards].hsetMap;
	{
		MutexLockGuard lock(mu);
		
		auto it = hsetMap.find(obj[0]);
		if(it == hsetMap.end())
		{
			addReply(session->sendBuf,shared.emptymultibulk);
			return false;
		}

		addReplyMultiBulkLen(session->sendBuf,it->second.size() * 2);

		for(auto iter = it->second.begin(); iter != it->second.end(); iter++)
		{
			addReplyBulkCBuffer(session->sendBuf,iter->first->ptr,sdsllen(iter->first->ptr));
			addReplyBulkCBuffer(session->sendBuf,iter->second->ptr,sdsllen(iter->second->ptr));
		}
	}

	zfree(obj[0]);

	return true;
}

bool xRedis::hlenCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  hlen error");
		return false;
	}

	obj[0]->calHash();
	size_t hash= obj[0]->hash;
	bool update = false;

	int count = 0;
	MutexLock &mu = hsetMapShards[hash% kShards].mutex;
	auto &hsetMap = hsetMapShards[hash% kShards].hsetMap;
	{
		MutexLockGuard lock(mu);
		auto it = hsetMap.find(obj[0]);
		if(it != hsetMap.end())
		{
			count = it->second.size();
		}
	}

	zfree(obj[0]);

	addReplyLongLong(session->sendBuf,count);
	return  true;
}

bool xRedis::hsetCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 3)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  hset error");
		return false;
	}

	obj[0]->calHash();
	obj[1]->calHash();
	size_t hash= obj[0]->hash;
	bool update = false;
	
	MutexLock &mu = hsetMapShards[hash% kShards].mutex;
	auto &hsetMap = hsetMapShards[hash% kShards].hsetMap;
	{
		MutexLockGuard lock(mu);
		auto it = hsetMap.find(obj[0]);
		if(it == hsetMap.end())
		{
			std::unordered_map<rObj*,rObj*,Hash,Equal> hset;
			hset.insert(std::make_pair(obj[1],obj[2]));
			hsetMap.insert(std::make_pair(obj[0],std::move(hset)));
		}
		else
		{
			zfree(obj[0]);
			auto iter = it->second.find(obj[1]);
			if(iter ==  it->second.end())
			{
				it->second.insert(std::make_pair(obj[1],obj[2]));
			}
			else
			{
				zfree(obj[1]);
				zfree(iter->second);
				iter->second = obj[2];
				update = true;
			}
		}
	}
	
	addReply(session->sendBuf,update ? shared.czero : shared.cone);

	return true;
}

bool xRedis::hgetCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() != 2)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  hget  param error");
		return false;
	}
	
	obj[0]->calHash();
	obj[1]->calHash();
	size_t hash= obj[0]->hash;
	MutexLock &mu = hsetMapShards[hash% kShards].mutex;
	auto &hsetMap = hsetMapShards[hash% kShards].hsetMap;
	{
		MutexLockGuard lock(mu);
		auto it = hsetMap.find(obj[0]);
		if(it == hsetMap.end())
		{
			addReply(session->sendBuf,shared.nullbulk);
			return false;
		}

		auto iter = it->second.find(obj[1]);
		if(iter == it->second.end())
		{
			addReply(session->sendBuf,shared.nullbulk);
			return false;
		}
		addReplyBulk(session->sendBuf,iter->second);
	}

	zfree(obj[0]);
	zfree(obj[1]);
	return true;
}



void xRedis::clearCommand()
{
	{
		for(auto it = setMapShards.begin(); it != setMapShards.end(); it++)
		{
			auto &map = (*it).setMap;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter !=map.end(); iter++)
			{
				auto iterr = expireTimers.find(iter->first);
				if(iterr != expireTimers.end())
				{
					expireTimers.erase(iterr);
					loop.cancelAfter(iterr->second);
				}
			
				zfree(iter->first);
				zfree(iter->second);
			}
			map.clear();
		}

	}

	{
		for(auto it = hsetMapShards.begin(); it != hsetMapShards.end(); it++)
		{
			auto &map = (*it).hsetMap;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter!=map.end(); iter++)
			{
				auto  &mmap = iter->second;
				for(auto iterr = mmap.begin(); iterr!=mmap.end(); iterr++)
				{
					zfree(iterr->first);
					zfree(iterr->second);
				}
				zfree(iter->first);
			}
			map.clear();

		}

	}

	{
		for(auto it = setShards.begin(); it != setShards.end(); it++)
		{
			auto &map = (*it).set;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter != map.end(); iter ++)
			{
				for(auto iterr = iter->second.begin(); iterr != iter->second.end() ;iterr ++)
				{
					zfree(*iterr);
				}
				zfree(iter->first);
			}
			map.clear();

		}
	}
	


	{
		for(auto it = sortSetShards.begin(); it != sortSetShards.end(); it++)
		{
			auto &map = (*it).set;
			auto &mmap = (*it).sset;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter != map.end(); iter ++)
			{
				for(auto iterr = iter->second.begin(); iterr != iter->second.end() ;iterr ++)
				{
						zfree(iterr->first);
						zfree(iterr->second);
				}

				zfree(iter->first);
			}

			mmap.clear();
			map.clear();

		}
	}


	{
		for(auto it = pubSubShards.begin(); it != pubSubShards.end(); it++)
		{
			auto &map = (*it).pubSub;
			MutexLock &mu =  (*it).mutex;
			MutexLockGuard lock(mu);
			for(auto iter = map.begin(); iter != map.end(); iter ++)
			{
				zfree(iter->first);
			}
			map.clear();

		}
	}

}


bool xRedis::flushdbCommand(const std::deque <rObj*> & obj,xSession * session)
{
	if(obj.size() > 0)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  flushdb  param error");
		return false;
	}

	clearCommand();

	addReply(session->sendBuf,shared.ok);	
	return true;
}

bool xRedis::quitCommand(const std::deque <rObj*> & obj,xSession * session)
{
	session->conn->forceClose();
	return true;
}


bool xRedis::setCommand(const std::deque <rObj*> & obj,xSession * session)
{	
	if(obj.size() <  2 || obj.size() > 8 )
	{
		addReplyErrorFormat(session->sendBuf,"unknown  set param error");
		return false;
	}

	int j;
	rObj *expire = nullptr;
	int unit = UNIT_SECONDS;	
	int flags = OBJ_SET_NO_FLAGS;

	for (j = 2; j < obj.size();j++)
	{
		const char *a = obj[j]->ptr;
		rObj *next = (j == obj.size() - 2) ? NULL : obj[j + 1];

		if ((a[0] == 'n' || a[0] == 'N') &&
		(a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
		!(flags & OBJ_SET_XX))
		{
			flags |= OBJ_SET_NX;
		}
		else if ((a[0] == 'x' || a[0] == 'X') &&
		       (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
		       !(flags & OBJ_SET_NX))
		{
			flags |= OBJ_SET_XX;
		}
		else if ((a[0] == 'e' || a[0] == 'E') &&
		       (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
		       !(flags & OBJ_SET_PX) && next)
		{
			flags |= OBJ_SET_EX;
			unit = UNIT_SECONDS;
			expire = next;
			j++;
		}
		else if ((a[0] == 'p' || a[0] == 'P') &&
		       (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
		       !(flags & OBJ_SET_EX) && next)
		{
			flags |= OBJ_SET_PX;
			unit = UNIT_MILLISECONDS;
			expire = next;
			j++;
		}
		else
		{
			addReply(session->sendBuf,shared.syntaxerr);
			return false;
		}
	}
	

	long long milliseconds = 0; /* initialized to avoid any harmness warning */

	if (expire)
	{
		if (getLongLongFromObjectOrReply(session->sendBuf, expire, &milliseconds, NULL) != REDIS_OK)
		   return false;
		if (milliseconds <= 0)
		{
		    addReplyErrorFormat(session->sendBuf,"invalid expire time in");
		    return false;
		}
		if (unit == UNIT_SECONDS) milliseconds *= 1000;
	}
    
	obj[0]->calHash();
	size_t hash= obj[0]->hash;
	int index = hash  % kShards;
	MutexLock &mu = setMapShards[index].mutex;
	auto & setMap = setMapShards[index].setMap;

	{
		MutexLockGuard lock(mu);

		auto it = setMap.find(obj[0]);
		if(it == setMap.end())
		{
			if(flags & OBJ_SET_XX)
			{
				addReply(session->sendBuf,shared.nullbulk);
				return false;
			}
			setMap.insert(std::make_pair(obj[0],obj[1]));
		}
		else
		{
			if(flags & OBJ_SET_NX)
			{
				addReply(session->sendBuf,shared.nullbulk);
				return false;
			}

			zfree(obj[0]);
			zfree(it->second);
			it->second = obj[1];
		}
	}

	if (expire)
	{
		MutexLockGuard lock(expireMutex);
		auto iter = expireTimers.find(obj[0]);
		if(iter != expireTimers.end())
		{
			expireTimers.erase(iter);
			loop.cancelAfter(iter->second);
		}

		xTimer * timer = loop.runAfter(milliseconds / 1000,(void *)(obj[0]),false,std::bind(&xRedis::handleSetExpire,this,std::placeholders::_1));
		expireTimers.insert(std::make_pair(obj[0],timer));

		for(int i = 2; i < obj.size(); i++)
		{
			zfree(obj[i]);
		}
	}

	addReply(session->sendBuf,shared.ok);
	return true;
}

bool xRedis::getCommand(const std::deque <rObj*> & obj,xSession * session)
{	
	if(obj.size() != 1)
	{
		addReplyErrorFormat(session->sendBuf,"unknown  get param error");
		return false;
	}
	
	obj[0]->calHash();
	size_t hash = obj[0]->hash;
	int index = hash  % kShards;
	MutexLock &mu = setMapShards[index].mutex;
	SetMap & setMap = setMapShards[index].setMap;
	{
		MutexLockGuard lock(mu);
		
		auto it = setMap.find(obj[0]);
		if(it == setMap.end())
		{
			addReply(session->sendBuf,shared.nullbulk);
			return false;
		}
		
		addReplyBulk(session->sendBuf,it->second);
	}

	zfree(obj[0]);
	
	return true;
}


void xRedis::flush()
{

}



void xRedis::initConifg()
{
	rObj * obj = createStringObject("set",3);
	handlerCommandMap[obj] =std::bind(&xRedis::setCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("get",3);
	handlerCommandMap[obj] =std::bind(&xRedis::getCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("flushdb",7);
	handlerCommandMap[obj] =std::bind(&xRedis::flushdbCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("dbsize",6);
	handlerCommandMap[obj] =std::bind(&xRedis::dbsizeCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("hset",4);
	handlerCommandMap[obj] =std::bind(&xRedis::hsetCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("hget",4);
	handlerCommandMap[obj] =std::bind(&xRedis::hgetCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("hgetall",7);
	handlerCommandMap[obj] =std::bind(&xRedis::hgetallCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("ping",4);
	handlerCommandMap[obj] =std::bind(&xRedis::pingCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("save",4);
	handlerCommandMap[obj] =std::bind(&xRedis::saveCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("slaveof",7);
	handlerCommandMap[obj] =std::bind(&xRedis::slaveofCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("sync",4);
	handlerCommandMap[obj] =std::bind(&xRedis::syncCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("command",7);
	handlerCommandMap[obj] =std::bind(&xRedis::commandCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("config",6);
	handlerCommandMap[obj] = std::bind(&xRedis::configCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("auth",4);
	handlerCommandMap[obj] = std::bind(&xRedis::authCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("info",4);
	handlerCommandMap[obj] = std::bind(&xRedis::infoCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("echo",4);
	handlerCommandMap[obj] = std::bind(&xRedis::echoCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("client",5);
	handlerCommandMap[obj] = std::bind(&xRedis::clientCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("subscribe",9);
	handlerCommandMap[obj] = std::bind(&xRedis::subscribeCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("hkeys",5);
	handlerCommandMap[obj] = std::bind(&xRedis::hkeysCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("select",6);
	handlerCommandMap[obj] = std::bind(&xRedis::selectCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("sadd",4);
	handlerCommandMap[obj] = std::bind(&xRedis::saddCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("scard",6);
	handlerCommandMap[obj] = std::bind(&xRedis::scardCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("publish",7);
	handlerCommandMap[obj] = std::bind(&xRedis::publishCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("del",3);
	handlerCommandMap[obj] = std::bind(&xRedis::delCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("unsubscribe",11);
	handlerCommandMap[obj] = std::bind(&xRedis::unsubscribeCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("hlen",4);
	handlerCommandMap[obj] = std::bind(&xRedis::hlenCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("zadd",4);
	handlerCommandMap[obj] = std::bind(&xRedis::zaddCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("zcard",5);
	handlerCommandMap[obj] = std::bind(&xRedis::zcardCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("zrange",6);
	handlerCommandMap[obj] = std::bind(&xRedis::zrangeCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("zrank",5);
	handlerCommandMap[obj] = std::bind(&xRedis::zrankCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("zrevrange",9);
	handlerCommandMap[obj] = std::bind(&xRedis::zrevrangeCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("keys",4);
	handlerCommandMap[obj] = std::bind(&xRedis::keysCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("bgsave",6);
	handlerCommandMap[obj] = std::bind(&xRedis::bgsaveCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("memory",6);
	handlerCommandMap[obj] = std::bind(&xRedis::memoryCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("ppong",6);
	handlerCommandMap[obj] = std::bind(&xRedis::ppongCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("pping", 6);
	handlerCommandMap[obj] = std::bind(&xRedis::ppingCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("sentinel", 8);
	handlerCommandMap[obj] = std::bind(&xRedis::ppingCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("cluster", 7);
	handlerCommandMap[obj] = std::bind(&xRedis::clusterCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("migrate", 7);
	handlerCommandMap[obj] = std::bind(&xRedis::migrateCommand, this, std::placeholders::_1, std::placeholders::_2);
	obj = createStringObject("set",3);
	unorderedmapCommands.insert(obj);
	obj = createStringObject("hset",4);
	unorderedmapCommands.insert(obj);
	obj = createStringObject("zadd",4);
	unorderedmapCommands.insert(obj);
	obj = createStringObject("sadd",4);
	unorderedmapCommands.insert(obj);
	obj = createStringObject("del",3);
	unorderedmapCommands.insert(obj);
	obj = createStringObject("flushdb",7);
	unorderedmapCommands.insert(obj);

	sentiThreads =  std::shared_ptr<std::thread>(new std::thread(std::bind(&xSentinel::connectSentinel,&senti)));
	sentiThreads->detach();
	repliThreads = std::shared_ptr<std::thread>(new std::thread(std::bind(&xReplication::connectMaster,&repli)));
	repliThreads->detach();
	clusterThreads = std::shared_ptr<std::thread>(new std::thread(std::bind(&xCluster::connectCluster, &clus)));
	clusterThreads->detach();

	
}


