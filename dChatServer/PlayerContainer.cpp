#include "PlayerContainer.h"
#include "dNetCommon.h"
#include <iostream>
#include <algorithm>
#include "Game.h"
#include "Logger.h"
#include "ChatPacketHandler.h"
#include "GeneralUtils.h"
#include "BitStreamUtils.h"
#include "Database.h"
#include "eConnectionType.h"
#include "ChatPackets.h"
#include "dConfig.h"
#include "MessageType/Chat.h"

void PlayerContainer::Initialize() {
	m_MaxNumberOfBestFriends =
		GeneralUtils::TryParse<uint32_t>(Game::config->GetValue("max_number_of_best_friends")).value_or(m_MaxNumberOfBestFriends);
	m_MaxNumberOfFriends =
		GeneralUtils::TryParse<uint32_t>(Game::config->GetValue("max_number_of_friends")).value_or(m_MaxNumberOfFriends);
}

TeamData::TeamData() {
	lootFlag = Game::config->GetValue("default_team_loot") == "0" ? 0 : 1;
}

void PlayerContainer::InsertPlayer(Packet* packet) {
	CINSTREAM_SKIP_HEADER;
	LWOOBJID playerId;
	if (!inStream.Read(playerId)) {
		LOG("Failed to read player ID");
		return;
	}

	auto isLogin = !m_Players.contains(playerId);
	auto& data = m_Players[playerId];
	data = PlayerData();
	data.isLogin = isLogin;
	data.playerID = playerId;

	uint32_t len;
	if (!inStream.Read<uint32_t>(len)) return;

	if (len > 33) {
		LOG("Received a really long player name, probably a fake packet %i.", len);
		return;
	}

	data.playerName.resize(len);
	inStream.ReadAlignedBytes(reinterpret_cast<unsigned char*>(data.playerName.data()), len);

	if (!inStream.Read(data.zoneID)) return;
	if (!inStream.Read(data.muteExpire)) return;
	if (!inStream.Read(data.gmLevel)) return;
	data.worldServerSysAddr = packet->systemAddress;

	m_Names[data.playerID] = GeneralUtils::UTF8ToUTF16(data.playerName);
	m_PlayerCount++;

	LOG("Added user: %s (%llu), zone: %i", data.playerName.c_str(), data.playerID, data.zoneID.GetMapID());

	Database::Get()->UpdateActivityLog(data.playerID, eActivityType::PlayerLoggedIn, data.zoneID.GetMapID());
	m_PlayersToRemove.erase(playerId);
}

void PlayerContainer::ScheduleRemovePlayer(Packet* packet) {
	CINSTREAM_SKIP_HEADER;
	LWOOBJID playerID{ LWOOBJID_EMPTY };
	inStream.Read(playerID);
	constexpr float updatePlayerOnLogoutTime = 20.0f;
	if (playerID != LWOOBJID_EMPTY) m_PlayersToRemove.insert_or_assign(playerID, updatePlayerOnLogoutTime);
}

void PlayerContainer::Update(const float deltaTime) {
	for (auto it = m_PlayersToRemove.begin(); it != m_PlayersToRemove.end();) {
		auto& [id, time] = *it;
		time -= deltaTime;

		if (time <= 0.0f) {
			RemovePlayer(id);
			it = m_PlayersToRemove.erase(it);
		} else {
			++it;
		}
	}
}

void PlayerContainer::RemovePlayer(const LWOOBJID playerID) {
	//Before they get kicked, we need to also send a message to their friends saying that they disconnected.
	const auto& player = GetPlayerData(playerID);

	if (!player) {
		LOG("Failed to find user: %llu", playerID);
		return;
	}

	for (const auto& fr : player.friends) {
		const auto& fd = this->GetPlayerData(fr.friendID);
		if (fd) ChatPacketHandler::SendFriendUpdate(fd, player, 0, fr.isBestFriend);
	}

	auto* team = GetTeam(playerID);

	if (team != nullptr) {
		const auto memberName = GeneralUtils::UTF8ToUTF16(player.playerName);

		for (const auto memberId : team->memberIDs) {
			const auto& otherMember = GetPlayerData(memberId);

			if (!otherMember) continue;

			ChatPacketHandler::SendTeamSetOffWorldFlag(otherMember, playerID, { 0, 0, 0 });
		}
	}

	m_PlayerCount--;
	LOG("Removed user: %llu", playerID);
	m_Players.erase(playerID);

	Database::Get()->UpdateActivityLog(playerID, eActivityType::PlayerLoggedOut, player.zoneID.GetMapID());
}

void PlayerContainer::MuteUpdate(Packet* packet) {
	CINSTREAM_SKIP_HEADER;
	LWOOBJID playerID;
	inStream.Read(playerID);
	time_t expire = 0;
	inStream.Read(expire);

	auto& player = this->GetPlayerDataMutable(playerID);

	if (!player) {
		LOG("Failed to find user: %llu", playerID);

		return;
	}

	player.muteExpire = expire;

	BroadcastMuteUpdate(playerID, expire);
}

void PlayerContainer::CreateTeamServer(Packet* packet) {
	CINSTREAM_SKIP_HEADER;
	LWOOBJID playerID;
	inStream.Read(playerID);
	size_t membersSize = 0;
	inStream.Read(membersSize);

	if (membersSize >= 4) {
		LOG("Tried to create a team with more than 4 players");
		return;
	}

	std::vector<LWOOBJID> members;

	members.reserve(membersSize);

	for (size_t i = 0; i < membersSize; i++) {
		LWOOBJID member;
		inStream.Read(member);
		members.push_back(member);
	}

	LWOZONEID zoneId;

	inStream.Read(zoneId);

	auto* team = CreateLocalTeam(members);

	if (team != nullptr) {
		team->zoneId = zoneId;
		UpdateTeamsOnWorld(team, false);
	}
}

void PlayerContainer::BroadcastMuteUpdate(LWOOBJID player, time_t time) {
	CBITSTREAM;
	BitStreamUtils::WriteHeader(bitStream, eConnectionType::CHAT, MessageType::Chat::GM_MUTE);

	bitStream.Write(player);
	bitStream.Write(time);

	Game::server->Send(bitStream, UNASSIGNED_SYSTEM_ADDRESS, true);
}

TeamData* PlayerContainer::CreateLocalTeam(std::vector<LWOOBJID> members) {
	if (members.empty()) {
		return nullptr;
	}

	TeamData* newTeam = nullptr;

	for (const auto member : members) {
		auto* team = GetTeam(member);

		if (team != nullptr) {
			RemoveMember(team, member, false, false, true);
		}

		if (newTeam == nullptr) {
			newTeam = CreateTeam(member, true);
		} else {
			AddMember(newTeam, member);
		}
	}

	newTeam->lootFlag = 1;

	TeamStatusUpdate(newTeam);

	return newTeam;
}

TeamData* PlayerContainer::CreateTeam(LWOOBJID leader, bool local) {
	auto* team = new TeamData();

	team->teamID = ++m_TeamIDCounter;
	team->leaderID = leader;
	team->local = local;

	GetTeamsMut().push_back(team);

	AddMember(team, leader);

	return team;
}

TeamData* PlayerContainer::GetTeam(LWOOBJID playerID) {
	for (auto* team : GetTeams()) {
		if (std::find(team->memberIDs.begin(), team->memberIDs.end(), playerID) == team->memberIDs.end()) continue;

		return team;
	}

	return nullptr;
}

void PlayerContainer::AddMember(TeamData* team, LWOOBJID playerID) {
	if (team->memberIDs.size() >= 4) {
		LOG("Tried to add player to team that already had 4 players");
		const auto& player = GetPlayerData(playerID);
		if (!player) return;
		ChatPackets::SendSystemMessage(player.worldServerSysAddr, u"The teams is full! You have not been added to a team!");
		return;
	}

	const auto index = std::find(team->memberIDs.begin(), team->memberIDs.end(), playerID);

	if (index != team->memberIDs.end()) return;

	team->memberIDs.push_back(playerID);

	const auto& leader = GetPlayerData(team->leaderID);
	const auto& member = GetPlayerData(playerID);

	if (!leader || !member) return;

	const auto leaderName = GeneralUtils::UTF8ToUTF16(leader.playerName);
	const auto memberName = GeneralUtils::UTF8ToUTF16(member.playerName);

	ChatPacketHandler::SendTeamInviteConfirm(member, false, leader.playerID, leader.zoneID, team->lootFlag, 0, 0, leaderName);

	if (!team->local) {
		ChatPacketHandler::SendTeamSetLeader(member, leader.playerID);
	} else {
		ChatPacketHandler::SendTeamSetLeader(member, LWOOBJID_EMPTY);
	}

	UpdateTeamsOnWorld(team, false);

	for (const auto memberId : team->memberIDs) {
		const auto& otherMember = GetPlayerData(memberId);

		if (otherMember == member) continue;

		const auto otherMemberName = GetName(memberId);

		ChatPacketHandler::SendTeamAddPlayer(member, false, team->local, false, memberId, otherMemberName, otherMember ? otherMember.zoneID : LWOZONEID(0, 0, 0));

		if (otherMember) {
			ChatPacketHandler::SendTeamAddPlayer(otherMember, false, team->local, false, member.playerID, memberName, member.zoneID);
		}
	}
}

void PlayerContainer::RemoveMember(TeamData* team, LWOOBJID causingPlayerID, bool disband, bool kicked, bool leaving, bool silent) {
	LOG_DEBUG("Player %llu is leaving team %i", causingPlayerID, team->teamID);
	const auto index = std::ranges::find(team->memberIDs, causingPlayerID);

	if (index == team->memberIDs.end()) return;

	team->memberIDs.erase(index);

	const auto& member = GetPlayerData(causingPlayerID);

	const auto causingMemberName = GetName(causingPlayerID);

	if (member && !silent) {
		ChatPacketHandler::SendTeamRemovePlayer(member, disband, kicked, leaving, team->local, LWOOBJID_EMPTY, causingPlayerID, causingMemberName);
	}

	if (team->memberIDs.size() <= 1) {
		DisbandTeam(team, causingPlayerID, causingMemberName);
	} else /* team has enough members to be a team still */ {
		team->leaderID = (causingPlayerID == team->leaderID) ? team->memberIDs[0] : team->leaderID;
		for (const auto memberId : team->memberIDs) {
			if (silent && memberId == causingPlayerID) {
				continue;
			}

			const auto& otherMember = GetPlayerData(memberId);

			if (!otherMember) continue;

			ChatPacketHandler::SendTeamRemovePlayer(otherMember, disband, kicked, leaving, team->local, team->leaderID, causingPlayerID, causingMemberName);
		}

		UpdateTeamsOnWorld(team, false);
	}
}

void PlayerContainer::PromoteMember(TeamData* team, LWOOBJID newLeader) {
	team->leaderID = newLeader;

	for (const auto memberId : team->memberIDs) {
		const auto& otherMember = GetPlayerData(memberId);

		if (!otherMember) continue;

		ChatPacketHandler::SendTeamSetLeader(otherMember, newLeader);
	}
}

void PlayerContainer::DisbandTeam(TeamData* team, const LWOOBJID causingPlayerID, const std::u16string& causingPlayerName) {
	const auto index = std::ranges::find(GetTeams(), team);

	if (index == GetTeams().end()) return;
	LOG_DEBUG("Disbanding team %i", (*index)->teamID);

	for (const auto memberId : team->memberIDs) {
		const auto& otherMember = GetPlayerData(memberId);

		if (!otherMember) continue;

		ChatPacketHandler::SendTeamSetLeader(otherMember, LWOOBJID_EMPTY);
		ChatPacketHandler::SendTeamRemovePlayer(otherMember, true, false, false, team->local, team->leaderID, causingPlayerID, causingPlayerName);
	}

	UpdateTeamsOnWorld(team, true);

	GetTeamsMut().erase(index);

	delete team;
}

void PlayerContainer::TeamStatusUpdate(TeamData* team) {
	const auto index = std::find(GetTeams().begin(), GetTeams().end(), team);

	if (index == GetTeams().end()) return;

	const auto& leader = GetPlayerData(team->leaderID);

	if (!leader) return;

	const auto leaderName = GeneralUtils::UTF8ToUTF16(leader.playerName);

	for (const auto memberId : team->memberIDs) {
		const auto& otherMember = GetPlayerData(memberId);

		if (!otherMember) continue;

		if (!team->local) {
			ChatPacketHandler::SendTeamStatus(otherMember, team->leaderID, leader.zoneID, team->lootFlag, 0, leaderName);
		}
	}

	UpdateTeamsOnWorld(team, false);
}

void PlayerContainer::UpdateTeamsOnWorld(TeamData* team, bool deleteTeam) {
	CBITSTREAM;
	BitStreamUtils::WriteHeader(bitStream, eConnectionType::CHAT, MessageType::Chat::TEAM_GET_STATUS);

	bitStream.Write(team->teamID);
	bitStream.Write(deleteTeam);

	if (!deleteTeam) {
		bitStream.Write(team->lootFlag);
		bitStream.Write<char>(team->memberIDs.size());
		for (const auto memberID : team->memberIDs) {
			bitStream.Write(memberID);
		}
	}

	Game::server->Send(bitStream, UNASSIGNED_SYSTEM_ADDRESS, true);
}

std::u16string PlayerContainer::GetName(LWOOBJID playerID) {
	const auto iter = m_Names.find(playerID);

	if (iter == m_Names.end()) return u"";

	return iter->second;
}

LWOOBJID PlayerContainer::GetId(const std::u16string& playerName) {
	LWOOBJID toReturn = LWOOBJID_EMPTY;

	for (const auto& [id, name] : m_Names) {
		if (name == playerName) {
			toReturn = id;
			break;
		}
	}

	return toReturn;
}

PlayerData& PlayerContainer::GetPlayerDataMutable(const LWOOBJID& playerID) {
	return m_Players.contains(playerID) ? m_Players[playerID] : m_Players[LWOOBJID_EMPTY];
}

PlayerData& PlayerContainer::GetPlayerDataMutable(const std::string& playerName) {
	for (auto& [id, player] : m_Players) {
		if (!player) continue;
		if (player.playerName == playerName) return player;
	}
	return m_Players[LWOOBJID_EMPTY];
}

const PlayerData& PlayerContainer::GetPlayerData(const LWOOBJID& playerID) {
	return GetPlayerDataMutable(playerID);
}

const PlayerData& PlayerContainer::GetPlayerData(const std::string& playerName) {
	return GetPlayerDataMutable(playerName);
}

void PlayerContainer::Shutdown() {
	m_Players.erase(LWOOBJID_EMPTY);
	while (!m_Players.empty()) {
		const auto& [id, playerData] = *m_Players.begin();
		Database::Get()->UpdateActivityLog(id, eActivityType::PlayerLoggedOut, playerData.zoneID.GetMapID());
		m_Players.erase(m_Players.begin());
	}
	for (auto* team : GetTeams()) if (team) delete team;
}
