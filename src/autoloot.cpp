/*
 * Auto-loot on kill for real (non-bot) players:
 *  - money goes straight to the bag, split only between real players of the loot-recipient group
 *    (bots don't take a share). Money taken from a loot window later follows the same rule;
 *  - quest items, and regular drops required by an incomplete quest in the log, are picked up
 *    while the player still needs them. Free-for-all quest items go to everyone who needs them; a drop
 *    that exists once goes to the real players who need it in turn (per group).
 * Everything else stays on the corpse for the normal loot window / group loot rules.
 *
 * Loot is generated in Unit::Kill before the kill hooks fire, so it is complete here.
 */

#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    bool IsRealPlayer(Player const* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsBot();
    }

    std::vector<Player*> GetAutoLooters(Creature* creature)
    {
        std::vector<Player*> looters;

        if (Group* group = creature->GetLootRecipientGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (Player* member = itr->GetSource())
                    if (IsRealPlayer(member) && member->IsInMap(creature) && member->IsAtLootRewardDistance(creature))
                        looters.push_back(member);
        }
        else if (Player* recipient = creature->GetLootRecipient())
        {
            if (IsRealPlayer(recipient) && recipient->IsInMap(creature) && recipient->IsAtLootRewardDistance(creature))
                looters.push_back(recipient);
        }

        return looters;
    }

    // A quest drop one of the looters may take: `slot` is that player's slot of the item (quest item slots are
    // per player, see Loot::LootItemInSlot).
    struct Claim
    {
        Player* player;
        uint32 slot;
    };

    // Per loot group: the real player who got the last contested quest drop. Kills run on map threads.
    std::mutex turnLock;
    std::unordered_map<uint32, ObjectGuid> lastReceiver;

    // Collects what `player` needs from the corpse. Free-for-all quest items are everyone's own copy: stored at
    // once. The rest (a quest item that drops once for the group, or a regular drop an incomplete quest still
    // needs, e.g. Chunk of Boar Meat for "Stocking Jetsteam") becomes a claim; `order` keeps first-seen order.
    void CollectQuestItems(Player* player, Loot& loot, std::vector<LootItem*>& order,
                           std::unordered_map<LootItem*, std::vector<Claim>>& claims)
    {
        auto claim = [&](LootItem* item, uint32 slot)
        {
            if (item->freeforall)
            {
                InventoryResult msg;
                player->StoreLootItem(uint8(slot), &loot, msg);
                return;
            }
            auto& list = claims[item];
            if (list.empty())
                order.push_back(item);
            list.push_back({ player, slot });
        };

        uint32 const maxSlot = loot.GetMaxSlotInLootFor(player);
        for (uint32 slot = loot.items.size(); slot < maxSlot; ++slot)
        {
            QuestItem* qitem = nullptr;
            LootItem* item = loot.LootItemInSlot(slot, player, &qitem);
            if (!item || !qitem || qitem->is_looted || item->is_looted)
                continue;

            // Only while the quest still needs it (quest active and objective not complete)
            if (!item->AllowedForPlayer(player, loot.sourceWorldObjectGUID))
                continue;

            claim(item, slot);
        }

        // Regular drops carry no quest flag, so without this group round-robin hands them to bots that loot
        // corpses first.
        for (uint32 slot = 0; slot < loot.items.size(); ++slot)
        {
            LootItem* item = loot.LootItemInSlot(slot, player);
            if (!item || item->is_looted || item->is_blocked || item->needs_quest)
                continue;

            if (!player->HasQuestForItem(item->itemid) || !item->AllowedForPlayer(player, loot.sourceWorldObjectGUID))
                continue;

            claim(item, slot);
        }
    }

    // One drop, several real players need it: they get such drops in turn (group order, starting after the
    // group's last receiver). A player whose bags are full is skipped for this drop.
    void GiveInTurn(Group* group, std::vector<Player*> const& looters, LootItem* item,
                    std::vector<Claim> const& list, Loot& loot)
    {
        auto indexOf = [&](ObjectGuid guid) -> int
        {
            for (size_t i = 0; i < looters.size(); ++i)
                if (looters[i]->GetGUID() == guid)
                    return int(i);
            return -1;
        };

        size_t start = 0;
        if (group && list.size() > 1)
        {
            ObjectGuid last;
            {
                std::lock_guard<std::mutex> guard(turnLock);
                auto it = lastReceiver.find(group->GetGUID().GetCounter());
                if (it != lastReceiver.end())
                    last = it->second;
            }
            // claims are in looter (group) order: the first one after the last receiver, else the first
            int const lastIndex = indexOf(last);
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (indexOf(list[i].player->GetGUID()) > lastIndex)
                {
                    start = i;
                    break;
                }
            }
        }

        for (size_t k = 0; k < list.size() && !item->is_looted; ++k)
        {
            Claim const& c = list[(start + k) % list.size()];
            // an earlier drop of this corpse may have completed the objective
            if (!item->AllowedForPlayer(c.player, loot.sourceWorldObjectGUID) ||
                (!item->needs_quest && !c.player->HasQuestForItem(item->itemid)))
                continue;

            InventoryResult msg;
            c.player->StoreLootItem(uint8(c.slot), &loot, msg);
            if (item->is_looted && group && list.size() > 1)
            {
                std::lock_guard<std::mutex> guard(turnLock);
                lastReceiver[group->GetGUID().GetCounter()] = c.player->GetGUID();
            }
        }
    }

    void LootQuestItems(Creature* creature, std::vector<Player*> const& looters, Loot& loot)
    {
        std::vector<LootItem*> order;
        std::unordered_map<LootItem*, std::vector<Claim>> claims;
        for (Player* player : looters)
            CollectQuestItems(player, loot, order, claims);

        Group* group = creature->GetLootRecipientGroup();
        for (LootItem* item : order)
            GiveInTurn(group, looters, item, claims[item], loot);
    }

    // Real players of the group who share money from `source`: those within loot reward distance, like
    // the core split. If none is near (a bot killed or looted far from everyone), all real players of
    // the group on that map, so the coins never end up with bots.
    std::vector<Player*> GetRealGroupMembers(Group* group, WorldObject const* source)
    {
        std::vector<Player*> nearby, onMap; // not `near`: that is a macro in windows headers
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!IsRealPlayer(member) || !member->IsInWorld() || !member->IsInMap(source))
                continue;

            onMap.push_back(member);
            if (member->IsAtLootRewardDistance(source))
                nearby.push_back(member);
        }
        return nearby.empty() ? onMap : nearby;
    }

    // Splits loot money evenly between `receivers` only. Copper left over by the division goes one
    // coin each to randomly chosen receivers, so nobody is favoured over time.
    void LootMoney(std::vector<Player*> const& receivers, Loot& loot)
    {
        if (!loot.gold || receivers.empty())
            return;

        uint32 const count = receivers.size();
        uint32 const share = loot.gold / count;
        uint32 const remainder = loot.gold % count;
        uint32 const firstExtra = urand(0, count - 1);
        loot.NotifyMoneyRemoved();
        loot.gold = 0;

        for (uint32 i = 0; i < count; ++i)
        {
            Player* player = receivers[i];
            bool const extra = (i + count - firstExtra) % count < remainder;
            uint32 const amount = share + (extra ? 1 : 0);
            if (!amount)
                continue;

            player->ModifyMoney(amount);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, amount);

            WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
            data << uint32(amount);
            data << uint8(receivers.size() > 1 ? 0 : 1); // 0 = "Your share is...", 1 = "You loot..."
            player->SendDirectMessage(&data);
        }
    }

    // Money receivers for a corpse: real players of its loot group, or the real looters otherwise.
    std::vector<Player*> GetMoneyReceivers(Creature* creature, std::vector<Player*> const& looters)
    {
        if (Group* group = creature->GetLootRecipientGroup())
            return GetRealGroupMembers(group, creature);
        return looters;
    }

    // Diagnostics: enable with `Logger.module.autoloot=5,Server` in worldserver.conf
    void AutoLoot(Creature* creature, Player* killer)
    {
        if (!creature)
            return;

        if (creature->IsPet() || !creature->isDead() || creature->IsLootRewardDisabled() ||
            !creature->IsDamageEnoughForLootingAndReward())
        {
            LOG_DEBUG("module.autoloot", "{} ({}): skip, pet={} dead={} rewardDisabled={} damageEnough={}",
                creature->GetName(), creature->GetEntry(), creature->IsPet(), creature->isDead(),
                creature->IsLootRewardDisabled(), creature->IsDamageEnoughForLootingAndReward());
            return;
        }

        Loot& loot = creature->loot;
        if (loot.isLooted())
        {
            LOG_DEBUG("module.autoloot", "{} ({}): skip, loot empty (gold={} unlooted={} items={})",
                creature->GetName(), creature->GetEntry(), loot.gold, loot.unlootedCount, loot.items.size());
            return;
        }

        std::vector<Player*> const looters = GetAutoLooters(creature);
        std::vector<Player*> const moneyReceivers = GetMoneyReceivers(creature, looters);
        if (looters.empty() && moneyReceivers.empty())
        {
            Player* recipient = creature->GetLootRecipient();
            LOG_DEBUG("module.autoloot", "{} ({}): skip, no real player (killer={} recipient={} recipientGroup={} gold={})",
                creature->GetName(), creature->GetEntry(), killer ? killer->GetName() : "-",
                recipient ? recipient->GetName() : "-", creature->GetLootRecipientGroup() ? "yes" : "no", loot.gold);
            return;
        }

        uint32 const gold = loot.gold;
        LootQuestItems(creature, looters, loot);

        LootMoney(moneyReceivers, loot);

        LOG_DEBUG("module.autoloot", "{} ({}): killer {}, item looters {}, money receivers {}, gold {} -> {}, unlooted left {}",
            creature->GetName(), creature->GetEntry(), killer ? killer->GetName() : "-", looters.size(),
            moneyReceivers.size(), gold, loot.gold, loot.unlootedCount);

        // Same cleanup as a normal loot release of an emptied corpse (removes sparkles, enables skinning)
        if (loot.isLooted())
        {
            creature->AllLootRemovedFromCorpse();
            creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            loot.clear();
        }
        else
            creature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
    }
}

class CustomAutoLootPlayerScript : public PlayerScript
{
public:
    CustomAutoLootPlayerScript() : PlayerScript("CustomAutoLootPlayerScript",
        { PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_CREATURE_KILLED_BY_PET }) { }

    // The killer may be a bot or a pet; auto-loot is decided by the corpse's loot recipients
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override { AutoLoot(killed, killer); }
    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override { AutoLoot(killed, owner); }
};

// Manual money looting (loot window, mod-aoe-loot merged windows, bots via their queued packet):
// in a group with real players the core would split the coins between every nearby member, bots
// included. Take over CMSG_LOOT_MONEY there and pay the real players only.
class CustomLootMoneyServerScript : public ServerScript
{
public:
    CustomLootMoneyServerScript() : ServerScript("CustomLootMoneyServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket& packet) override
    {
        if (packet.GetOpcode() != CMSG_LOOT_MONEY)
            return true;

        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
            return true;

        Group* group = player->GetGroup();
        if (!group)
            return true;

        // Same source checks as WorldSession::HandleLootMoneyOpcode for the shared-money cases;
        // anything else (items, pickpocket, player corpses) is left to the core.
        ObjectGuid const guid = player->GetLootGUID();
        Loot* loot = nullptr;
        WorldObject* source = nullptr;
        if (guid.IsGameObject())
        {
            GameObject* go = player->GetMap()->GetGameObject(guid);
            if (go && (go->GetOwnerGUID() == player->GetGUID() || go->IsWithinDistInMap(player)))
            {
                loot = &go->loot;
                source = go;
            }
        }
        else if (guid.IsCreatureOrVehicle())
        {
            Creature* creature = player->GetMap()->GetCreature(guid);
            if (creature && !creature->IsAlive() && creature->IsWithinDistInMap(player, INTERACTION_DISTANCE))
            {
                loot = &creature->loot;
                source = creature;
            }
        }

        if (!loot || !loot->gold)
            return true;

        // Shares are measured from the corpse / chest, not from whoever clicked (it may be a bot)
        std::vector<Player*> const receivers = GetRealGroupMembers(group, source);
        if (receivers.empty())
            return true; // bot-only group: normal split

        sScriptMgr->OnPlayerBeforeLootMoney(player, loot);
        uint32 const gold = loot->gold;
        LootMoney(receivers, *loot);
        sScriptMgr->OnLootMoney(player, gold);

        LOG_DEBUG("module.autoloot", "loot window money {} by {}: paid to {} real player(s)", gold, player->GetName(),
            receivers.size());
        return false;
    }
};

void AddCustomAutoLootScripts()
{
    new CustomAutoLootPlayerScript();
    new CustomLootMoneyServerScript();
}
