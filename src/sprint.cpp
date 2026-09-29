/*
 * Sprint for all players (and playerbots).
 *
 * The spell is the plugin's own: data/patches.json adds a copy of spell 56354 with 20 s, +50% speed and a 35 s
 * cooldown to the client and the server (spell_dbc), under an id given out when the patch was installed.
 */

#include "DatabaseEnv.h"
#include "Log.h"
#include "MoveSpline.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"

namespace
{
    // The id of the Sprint spell (world.plugin_ids), 0 while the plugin's patches are not installed.
    uint32 SprintSpell()
    {
        static uint32 const id = []
        {
            QueryResult r = WorldDatabase.Query("SELECT `id` FROM `plugin_ids` WHERE `plugin` = 'lonelyice.qol' AND `name` = 'sprint'");
            uint32 v = r ? r->Fetch()[0].Get<uint32>() : 0;
            if (!v)
                LOG_ERROR("module", "lonelyice.qol: no id for the Sprint spell, its patches are not installed");
            return v;
        }();
        return id;
    }
}

// Looks the spell id up before the world runs (the first lookup is a database query).
class CustomSprintWorldScript : public WorldScript
{
public:
    CustomSprintWorldScript() : WorldScript("CustomSprintWorldScript", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        SprintSpell();
    }
};
// Teaches the spell to every character (existing and new, including bots) on login
class CustomSprintPlayerScript : public PlayerScript
{
public:
    CustomSprintPlayerScript() : PlayerScript("CustomSprintPlayerScript", { PLAYERHOOK_ON_LOGIN }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (SprintSpell() && !player->HasSpell(SprintSpell()))
            player->learnSpell(SprintSpell());
    }
};

/*
 * Bots out of combat keep up with their (real) master:
 *  - echo: while the master is sprinting, a following bot gets the same Sprint aura for the master's remaining
 *    time, without a cooldown; everyone runs at the same speed, so the formation holds and nobody overtakes;
 *  - catch-up: a bot that falls behind further than CATCHUP_DIST while moving casts its own Sprint (real cast,
 *    real cooldown);
 *  - no overtaking: a catch-up sprint is cancelled once the bot is back within STOP_DIST and the master is not
 *    sprinting.
 */
namespace
{
    constexpr uint32 CHECK_MS = 500;
    constexpr float CATCHUP_DIST = 25.0f;
    constexpr float STOP_DIST = 8.0f;

    struct BotSprintState : public DataMap::Base
    {
        uint32 timer = 0;
        bool echo = false;       // the bot's Sprint aura is an echo of the master's
    };

    Player* RealMaster(Player* bot)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        Player* master = ai ? ai->GetMaster() : nullptr;
        if (!master || master == bot || !master->GetSession() || master->GetSession()->IsBot())
            return nullptr;

        return master;
    }
}

class CustomBotSprintPlayerScript : public PlayerScript
{
public:
    CustomBotSprintPlayerScript() : PlayerScript("CustomBotSprintPlayerScript", { PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerUpdate(Player* bot, uint32 diff) override
    {
        WorldSession* session = bot->GetSession();
        if (!session || !session->IsBot())
            return;

        if (!SprintSpell())
            return;

        BotSprintState* st = bot->CustomData.GetDefault<BotSprintState>("custom_bot_sprint");
        st->timer += diff;
        if (st->timer < CHECK_MS)
            return;
        st->timer = 0;

        Aura* own = bot->GetAura(SprintSpell());
        if (!own)
            st->echo = false;

        Player* master = RealMaster(bot);
        bool const usable = master && bot->IsInWorld() && master->IsInWorld() && bot->IsAlive() && master->IsAlive() &&
            bot->GetMap() == master->GetMap() && !bot->IsInCombat() && !master->IsInCombat() &&
            !bot->IsInFlight() && !bot->IsBeingTeleported();

        if (!usable)
        {
            if (own && st->echo)
                bot->RemoveAurasDueToSpell(SprintSpell());
            return;
        }

        Aura* masterSprint = master->GetAura(SprintSpell());
        float const dist = bot->GetDistance(master);

        // echo the master's sprint (same remaining time, no cooldown)
        if (masterSprint)
        {
            if (!own)
            {
                if (Aura* echo = bot->AddAura(SprintSpell(), bot))
                {
                    echo->SetMaxDuration(masterSprint->GetDuration());
                    echo->SetDuration(masterSprint->GetDuration());
                    st->echo = true;
                }
            }
            return;
        }

        if (own)
        {
            // the master's sprint ended: drop the echo; a catch-up sprint ends once the bot is close again
            if (st->echo || dist <= STOP_DIST)
            {
                bot->RemoveAurasDueToSpell(SprintSpell());
                st->echo = false;
            }
            return;
        }

        // catch-up: far behind and following (a spline in progress), own sprint off cooldown
        bool const moving = bot->movespline && !bot->movespline->Finalized();
        if (dist > CATCHUP_DIST && moving && !bot->HasSpellCooldown(SprintSpell()) &&
            !bot->IsNonMeleeSpellCast(false) && bot->HasSpell(SprintSpell()))
            bot->CastSpell(bot, SprintSpell(), false);
    }
};

void AddCustomSprintScripts()
{
    new CustomSprintWorldScript();
    new CustomSprintPlayerScript();
    new CustomBotSprintPlayerScript();
}
