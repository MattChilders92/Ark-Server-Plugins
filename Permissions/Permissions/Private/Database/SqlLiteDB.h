#pragma once

#include <SQLiteCpp/Database.h>

#include "IDatabase.h"
#include "../Main.h"

class SqlLite : public IDatabase
{
public:
	explicit SqlLite(const std::string& path)
		: db_(path.empty()
			? Permissions::GetDbPath()
			: path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
	{
		try
		{
			db_.exec("PRAGMA journal_mode=WAL;");

			db_.exec("create table if not exists Players ("
				"Id integer primary key autoincrement not null,"
				"SteamId integer default 0,"
				"Groups text default 'Default,' COLLATE NOCASE,"
				"TimedGroups text default '' COLLATE NOCASE"
				");");
			db_.exec("create table if not exists Tribes ("
				"Id integer primary key autoincrement not null,"
				"TribeId integer default 0,"
				"Groups text default '' COLLATE NOCASE,"
				"TimedGroups text default '' COLLATE NOCASE"
				");");
			db_.exec("create table if not exists Groups ("
				"Id integer primary key autoincrement not null,"
				"GroupName text not null COLLATE NOCASE,"
				"Permissions text default '' COLLATE NOCASE"
				");");
			// Add default groups

			db_.exec("INSERT INTO Groups(GroupName, Permissions)"
				"SELECT 'Admins', '*,'"
				"WHERE NOT EXISTS(SELECT 1 FROM Groups WHERE GroupName = 'Admins');");
			db_.exec("INSERT INTO Groups(GroupName)"
				"SELECT 'Default'"
				"WHERE NOT EXISTS(SELECT 1 FROM Groups WHERE GroupName = 'Default');");

			upgradeDatabase();
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
		}
	}

	bool AddPlayer(uint64 steam_id) override
	{
		try
		{
			SQLite::Statement query(db_, "INSERT INTO Players (SteamId) VALUES (?);");
			query.bind(1, static_cast<int64>(steam_id));
			query.exec();

			std::lock_guard<std::mutex> lg(playersMutex);
			permissionPlayers[steam_id] = CachedPermission("Default,", "");

			return true;
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return false;
		}

		return false;
	}

	bool IsPlayerExists(uint64 steam_id) override
	{
		return permissionPlayers.count(steam_id) > 0;
	}

	bool IsGroupExists(const FString& group) override
	{
		return permissionGroups.count(group.ToString()) > 0;
	}

	TArray<FString> GetPlayerGroups(uint64 steam_id) override
	{
		TArray<FString> groups;
		auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		if (permissionPlayers.count(steam_id) > 0)
		{
			groups = permissionPlayers[steam_id].getGroups(nowSecs);
		}
		return groups;
	}

	TArray<FString> GetGroupPermissions(const FString& group) override
	{
		if (group.IsEmpty())
			return {};

		TArray<FString> permissions;

		if (permissionGroups.count(group.ToString()) > 0)
		{
			FString permissions_fstr(permissionGroups[group.ToString()]);
			permissions_fstr.ParseIntoArray(permissions, L",", true);
		}

		return permissions;
	}

	TArray<FString> GetAllGroups() override
	{
		TArray<FString> all_groups;

		for (auto& group : permissionGroups)
		{
			all_groups.Add(group.first.c_str());
		}

		return all_groups;
	}

	TArray<uint64> GetGroupMembers(const FString& group) override
	{
		TArray<uint64> members;

		for (auto& players : permissionPlayers)
		{
			if (Permissions::IsPlayerInGroup(players.first, group))
				members.Add(players.first);
		}

		return members;
	}

	std::optional<std::string> AddPlayerToGroup(uint64 steam_id, const FString& group) override
	{
		if (!IsPlayerExists(steam_id))
			AddPlayer(steam_id);

		if (!IsGroupExists(group))
			return  "Group does not exist";

		if (Permissions::IsPlayerInGroup(steam_id, group))
			return "Player was already added";

		TArray<FString> groups = GetPlayerGroups(steam_id);

		FString new_groups;

		for (const FString& current_group : groups)
		{
			new_groups += current_group + ",";
		}
		new_groups += group + ",";
		try
		{
			SQLite::Statement query(db_, "UPDATE Players SET Groups = ? WHERE SteamId = ?;");
			query.bind(1, new_groups.ToString());
			query.bind(2, static_cast<int64>(steam_id));
			query.exec();

			std::lock_guard<std::mutex> lg(playersMutex);
			permissionPlayers[steam_id].Groups.Add(group);
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemovePlayerFromGroup(uint64 steam_id, const FString& group) override
	{
		if (!IsPlayerExists(steam_id) || !IsGroupExists(group))
			return "Player or group does not exist";

		if (!Permissions::IsPlayerInGroup(steam_id, group))
			return "Player is not in group";

		TArray<FString> groups = GetPlayerGroups(steam_id);

		FString new_groups;

		for (const FString& current_group : groups)
		{
			if (current_group != group)
				new_groups += current_group + ",";
		}

		try
		{
			SQLite::Statement query(db_, "UPDATE Players SET Groups = ? WHERE SteamId = ?;");
			query.bind(1, new_groups.ToString());
			query.bind(2, static_cast<int64>(steam_id));
			query.exec();

			std::lock_guard<std::mutex> lg(playersMutex);
			permissionPlayers[steam_id].Groups.Remove(group);
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> AddPlayerToTimedGroup(uint64 steam_id, const FString& group, int secs, int delaySecs) override
	{
		if (!IsPlayerExists(steam_id))
			AddPlayer(steam_id);

		if (!IsGroupExists(group))
			return  "Group does not exist";

		TArray<TimedGroup> groups;
		if (permissionPlayers.count(steam_id) > 0)
		{
			groups = permissionPlayers[steam_id].TimedGroups;
		}
		for (int32 Index = groups.Num() - 1; Index >= 0; --Index)
		{
			const TimedGroup& current_group = groups[Index];
			if (current_group.GroupName.Equals(group)) {
				groups.RemoveAt(Index);
				continue;
			}
		}
		if (Permissions::IsPlayerInGroup(steam_id, group))
			return "Player is already permanetly in this group.";
		long long ExpireAtSecs = 0;
		long long delayUntilSecs = 0;
		if (delaySecs > 0) {
			delayUntilSecs = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::seconds(delaySecs)).time_since_epoch()).count();
		}
		ExpireAtSecs = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::seconds(secs)).time_since_epoch()).count();

		groups.Add(TimedGroup{ group, delayUntilSecs, ExpireAtSecs });
		FString new_groups;
		for (const TimedGroup& current_group : groups)
		{
			new_groups += FString::Format("{};{};{},", current_group.DelayUntilTime, current_group.ExpireAtTime, current_group.GroupName.ToString());
		}

		try
		{
			SQLite::Statement query(db_, "UPDATE Players SET TimedGroups = ? WHERE SteamId = ?;");
			query.bind(1, new_groups.ToString());
			query.bind(2, static_cast<int64>(steam_id));
			query.exec();

			std::lock_guard<std::mutex> lg(playersMutex);
			permissionPlayers[steam_id].TimedGroups = groups;
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemovePlayerFromTimedGroup(uint64 steam_id, const FString& group) override
	{
		if (!IsPlayerExists(steam_id) || !IsGroupExists(group))
			return "Player or group does not exist";

		TArray<TimedGroup> groups = permissionPlayers[steam_id].TimedGroups;

		FString new_groups;
		int32 groupIndex = INDEX_NONE;
		for (int32 Index = 0; Index != groups.Num(); ++Index)
		{
			const TimedGroup& current_group = groups[Index];
			if (current_group.GroupName != group)
				new_groups += FString::Format("{};{};{},", current_group.DelayUntilTime, current_group.ExpireAtTime, current_group.GroupName.ToString());
			else
				groupIndex = Index;
		}
		if (groupIndex == INDEX_NONE)
			return "Player is not in timed group";

		try
		{
			SQLite::Statement query(db_, "UPDATE Players SET TimedGroups = ? WHERE SteamId = ?;");
			query.bind(1, new_groups.ToString());
			query.bind(2, static_cast<int64>(steam_id));
			query.exec();

			std::lock_guard<std::mutex> lg(playersMutex);
			permissionPlayers[steam_id].TimedGroups.RemoveAt(groupIndex);
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> AddGroup(const FString& group) override
	{
		if (IsGroupExists(group))
			return "Group already exists";

		try
		{
			SQLite::Statement query(db_, "INSERT INTO Groups (GroupName) VALUES (?);");
			query.bind(1, group.ToString());
			query.exec();

			std::lock_guard<std::mutex> lg(groupsMutex);
			permissionGroups[group.ToString()] = "";
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemoveGroup(const FString& group) override
	{
		if (!IsGroupExists(group))
			return "Group does not exist";

		// Remove all players from this group
		TArray<uint64> group_members = GetGroupMembers(group);
		for (uint64 player : group_members)
		{
			RemovePlayerFromGroup(player, group);
		}
		// Delete group

		try
		{
			SQLite::Statement query(db_, "DELETE FROM Groups WHERE GroupName = ?;");
			query.bind(1, group.ToString());
			query.exec();

			std::lock_guard<std::mutex> lg(groupsMutex);
			permissionGroups.erase(group.ToString());
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> GroupGrantPermission(const FString& group, const FString& permission) override
	{
		if (!IsGroupExists(group))
			return "Group does not exist";

		if (Permissions::IsGroupHasPermission(group, permission))
			return "Group already has this permission";

		try
		{
			SQLite::Statement
				query(db_, "UPDATE Groups SET Permissions = Permissions || ? || ',' WHERE GroupName = ?;");
			query.bind(1, permission.ToString());
			query.bind(2, group.ToString());
			query.exec();

			std::lock_guard<std::mutex> lg(groupsMutex);
			std::string groupPermissions = fmt::format("{},{}", permission.ToString(), permissionGroups[group.ToString()]);
			permissionGroups[group.ToString()] = groupPermissions;
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> GroupRevokePermission(const FString& group, const FString& permission) override
	{
		if (!IsGroupExists(group))
			return "Group does not exist";

		if (!Permissions::IsGroupHasPermission(group, permission))
			return "Group does not have this permission";

		TArray<FString> permissions = GetGroupPermissions(group);

		FString new_permissions;

		for (const FString& current_perm : permissions)
		{
			if (current_perm != permission)
				new_permissions += current_perm + ",";
		}

		try
		{
			SQLite::Statement query(db_, "UPDATE Groups SET Permissions = ? WHERE GroupName = ?;");
			query.bind(1, new_permissions.ToString());
			query.bind(2, group.ToString());
			query.exec();

			std::lock_guard<std::mutex> lg(groupsMutex);
			permissionGroups[group.ToString()] = new_permissions.ToString();
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	void Init() override
	{
		groupsMutex.lock();
		permissionGroups = InitGroups();
		groupsMutex.unlock();

		playersMutex.lock();
		permissionPlayers = InitPlayers();
		playersMutex.unlock();

		tribesMutex.lock();
		permissionTribes = InitTribes();
		tribesMutex.unlock();
	}

	std::unordered_map<std::string, std::string> InitGroups() override
	{
		std::unordered_map<std::string, std::string> pGroups;

		try
		{
			SQLite::Statement query(db_, "SELECT GroupName, Permissions FROM Groups;");
			while (query.executeStep())
			{
				pGroups[query.getColumn(0).getText()] = query.getColumn(1).getText();
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
		}

		return pGroups;
	}

	std::unordered_map<uint64, CachedPermission> InitPlayers() override
	{
		std::unordered_map<uint64, CachedPermission> pPlayers;

		try
		{
			SQLite::Statement query(db_, "SELECT SteamId, Groups, TimedGroups FROM Players;");
			if (query.executeStep())
			{
				uint64 steam_id = query.getColumn(0).getInt64();
				FString Groups = query.getColumn(1).getText();
				FString TimedGroups = query.getColumn(2).getText();
				pPlayers[steam_id] = CachedPermission(Groups, TimedGroups);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
		}

		return pPlayers;
	}

	bool AddTribe(int tribeId) override
	{
		try
		{
			SQLite::Statement query(db_, "INSERT INTO Tribes (TribeId) VALUES (?);");
			query.bind(1, static_cast<int64>(tribeId));
			query.exec();

			std::lock_guard<std::mutex> lg(tribesMutex);
			permissionTribes[tribeId] = CachedPermission("", "");

			return true;
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return false;
		}

		return false;
	}

	bool IsTribeExists(int tribeId) override
	{
		return permissionTribes.count(tribeId) > 0;
	}

	TArray<FString> GetTribeGroups(int tribeId) override
	{
		TArray<FString> groups;
		auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		if (permissionTribes.count(tribeId) > 0)
		{
			groups = permissionTribes[tribeId].getGroups(nowSecs);
		}
		return groups;
	}

	std::optional<std::string> AddTribeToGroup(int tribeId, const FString& group) override
	{
		if (!IsTribeExists(tribeId))
			AddTribe(tribeId);

		if (!IsGroupExists(group))
			return  "Group does not exist";

		if (Permissions::IsTribeInGroup(tribeId, group))
			return "Tribe was already added";

		try
		{
			SQLite::Statement query(db_, "UPDATE Tribes SET Groups = Groups || ? || ',' WHERE TribeId = ?;");
			query.bind(1, group.ToString());
			query.bind(2, static_cast<int64>(tribeId));
			query.exec();

			std::lock_guard<std::mutex> lg(tribesMutex);
			permissionTribes[tribeId].Groups.Add(group);
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemoveTribeFromGroup(int tribeId, const FString& group) override
	{
		if (!IsTribeExists(tribeId) || !IsGroupExists(group))
			return "Tribe or group does not exist";

		if (!Permissions::IsTribeInGroup(tribeId, group))
			return "Tribe is not in group";

		TArray<FString> groups = GetTribeGroups(tribeId);

		FString new_groups;

		for (const FString& current_group : groups)
		{
			if (current_group != group)
				new_groups += current_group + ",";
		}

		try
		{
			SQLite::Statement query(db_, "UPDATE Tribes SET Groups = ? WHERE TribeId = ?;");
			query.bind(1, new_groups.ToString());
			query.bind(2, static_cast<int64>(tribeId));
			query.exec();

			std::lock_guard<std::mutex> lg(tribesMutex);
			permissionTribes[tribeId].Groups.Remove(group);
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> AddTribeToTimedGroup(int tribeId, const FString& group, int secs, int delaySecs) override
	{
		if (!IsTribeExists(tribeId))
			AddTribe(tribeId);

		if (!IsGroupExists(group))
			return  "Group does not exist";

		TArray<TimedGroup> groups;
		if (permissionTribes.count(tribeId) > 0)
		{
			groups = permissionTribes[tribeId].TimedGroups;
		}
		for (int32 Index = groups.Num() - 1; Index >= 0; --Index)
		{
			const TimedGroup& current_group = groups[Index];
			if (current_group.GroupName.Equals(group)) {
				groups.RemoveAt(Index);
				continue;
			}
		}
		if (Permissions::IsTribeInGroup(tribeId, group))
			return "Tribe is already permanetly in this group.";
		long long ExpireAtSecs = 0;
		long long delayUntilSecs = 0;
		if (delaySecs > 0) {
			delayUntilSecs = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::seconds(delaySecs)).time_since_epoch()).count();
		}
		ExpireAtSecs = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::system_clock::now() + std::chrono::seconds(secs)).time_since_epoch()).count();

		groups.Add(TimedGroup{ group, delayUntilSecs, ExpireAtSecs });
		FString new_groups;
		for (const TimedGroup& current_group : groups)
		{
			new_groups += FString::Format("{};{};{},", current_group.DelayUntilTime, current_group.ExpireAtTime, current_group.GroupName.ToString());
		}

		try
		{
			SQLite::Statement query(db_, "UPDATE Tribes SET TimedGroups = ? WHERE TribeId = ?;");
			query.bind(1, new_groups.ToString());
			query.bind(2, static_cast<int64>(tribeId));
			query.exec();

			std::lock_guard<std::mutex> lg(tribesMutex);
			permissionTribes[tribeId].TimedGroups = groups;
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::optional<std::string> RemoveTribeFromTimedGroup(int tribeId, const FString& group) override
	{
		if (!IsTribeExists(tribeId) || !IsGroupExists(group))
			return "Tribe or group does not exist";

		TArray<TimedGroup> groups = permissionTribes[tribeId].TimedGroups;

		FString new_groups;
		int32 groupIndex = INDEX_NONE;
		for (int32 Index = 0; Index != groups.Num(); ++Index)
		{
			const TimedGroup& current_group = groups[Index];
			if (current_group.GroupName != group)
				new_groups += FString::Format("{};{};{},", current_group.DelayUntilTime, current_group.ExpireAtTime, current_group.GroupName.ToString());
			else
				groupIndex = Index;
		}
		if (groupIndex == INDEX_NONE)
			return "Tribe is not in timed group";

		try
		{
			SQLite::Statement query(db_, "UPDATE Tribes SET TimedGroups = ? WHERE TribeId = ?;");
			query.bind(1, new_groups.ToString());
			query.bind(2, static_cast<int64>(tribeId));
			query.exec();

			std::lock_guard<std::mutex> lg(tribesMutex);
			permissionTribes[tribeId].TimedGroups.RemoveAt(groupIndex);
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			return "Unexpected DB error";
		}

		return {};
	}

	std::unordered_map<int, CachedPermission> InitTribes() override
	{
		std::unordered_map<int, CachedPermission> pTribes;

		try
		{
			SQLite::Statement query(db_, "SELECT TribeId, Groups, TimedGroups FROM Tribes;");
			if (query.executeStep())
			{
				int tribeId = query.getColumn(0).getInt();
				FString Groups = query.getColumn(1).getText();
				FString TimedGroups = query.getColumn(2).getText();
				pTribes[tribeId] = CachedPermission(Groups, TimedGroups);
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
		}

		return pTribes;
	}

	void upgradeDatabase() {
		SQLite::Statement query(db_, "PRAGMA table_info('Players')");
		int cols = 0;
		while (query.executeStep()) {
			cols++;
		}
		if (cols == 3) {
			db_.exec("ALTER TABLE players ADD COLUMN TimedGroups text default '' COLLATE NOCASE;");
			Log::GetLog()->warn("Upgraded Permissions DB Tables.");
		}
	}

private:
	SQLite::Database db_;
};