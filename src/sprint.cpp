/*
 * Custom Sprint for all players (and playerbots).
 *
 * Reuses client-known spell 56354 "Sprint" (+200% speed, 15s, no cooldown in DBC; otherwise
 * only used by a few NPCs). Its values are overridden here only when a player casts it, so
 * NPC usage stays untouched. Because the client's Spell.dbc has no cooldown for this spell,
 * the cooldown is sent to the client explicitly.
 *
 * Known limitation: the client tooltip still shows the original DBC text (200% / 15 sec).
 */

#include "MoveSpline.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

enum CustomSprint
{
    SPELL_CUSTOM_SPRINT    = 56354,
    SPRINT_DURATION_MS     = 20 * IN_MILLISECONDS,
    SPRINT_COOLDOWN_MS     = 35 * IN_MILLISECONDS,
    SPRINT_SPEED_BONUS_PCT = 50
};

class spell_custom_sprint : public SpellScript
{
    PrepareSpellScript(spell_custom_sprint);

    void HandleAfterHit()
    {
        if (!GetCaster()->IsPlayer())
            return;

        if (Aura* aura = GetHitAura())
        {
            aura->SetMaxDuration(SPRINT_DURATION_MS);
            aura->SetDuration(SPRINT_DURATION_MS);
        }
    }

    void HandleAfterCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return;

        player->AddSpellCooldown(SPELL_CUSTOM_SPRINT, 0, SPRINT_COOLDOWN_MS);

        WorldPacket data;
        player->BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, SPELL_CUSTOM_SPRINT, SPRINT_COOLDOWN_MS);
        player->SendDirectMessage(&data);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_custom_sprint::HandleAfterHit);
        AfterCast += SpellCastFn(spell_custom_sprint::HandleAfterCast);
    }
};

class spell_custom_sprint_aura : public AuraScript
{
    PrepareAuraScript(spell_custom_sprint_aura);

    void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (Unit* caster = GetCaster())
            if (caster->IsPlayer())
                amount = SPRINT_SPEED_BONUS_PCT;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_custom_sprint_aura::CalcAmount, EFFECT_0, SPELL_AURA_MOD_INCREASE_SPEED);
    }
};

// Teaches the spell to every character (existing and new, including bots) on login
class CustomSprintPlayerScript : public PlayerScript
{
public:
    CustomSprintPlayerScript() : PlayerScript("CustomSprintPlayerScript", { PLAYERHOOK_ON_LOGIN }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!player->HasSpell(SPELL_CUSTOM_SPRINT))
            player->learnSpell(SPELL_CUSTOM_SPRINT);
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

        BotSprintState* st = bot->CustomData.GetDefault<BotSprintState>("custom_bot_sprint");
        st->timer += diff;
        if (st->timer < CHECK_MS)
            return;
        st->timer = 0;

        Aura* own = bot->GetAura(SPELL_CUSTOM_SPRINT);
        if (!own)
            st->echo = false;

        Player* master = RealMaster(bot);
        bool const usable = master && bot->IsInWorld() && master->IsInWorld() && bot->IsAlive() && master->IsAlive() &&
            bot->GetMap() == master->GetMap() && !bot->IsInCombat() && !master->IsInCombat() &&
            !bot->IsInFlight() && !bot->IsBeingTeleported();

        if (!usable)
        {
            if (own && st->echo)
                bot->RemoveAurasDueToSpell(SPELL_CUSTOM_SPRINT);
            return;
        }

        Aura* masterSprint = master->GetAura(SPELL_CUSTOM_SPRINT);
        float const dist = bot->GetDistance(master);

        // echo the master's sprint (same remaining time, no cooldown)
        if (masterSprint)
        {
            if (!own)
            {
                if (Aura* echo = bot->AddAura(SPELL_CUSTOM_SPRINT, bot))
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
                bot->RemoveAurasDueToSpell(SPELL_CUSTOM_SPRINT);
                st->echo = false;
            }
            return;
        }

        // catch-up: far behind and following (a spline in progress), own sprint off cooldown
        bool const moving = bot->movespline && !bot->movespline->Finalized();
        if (dist > CATCHUP_DIST && moving && !bot->HasSpellCooldown(SPELL_CUSTOM_SPRINT) &&
            !bot->IsNonMeleeSpellCast(false) && bot->HasSpell(SPELL_CUSTOM_SPRINT))
            bot->CastSpell(bot, SPELL_CUSTOM_SPRINT, false);
    }
};

void AddCustomSprintScripts()
{
    RegisterSpellAndAuraScriptPair(spell_custom_sprint, spell_custom_sprint_aura);
    new CustomSprintPlayerScript();
    new CustomBotSprintPlayerScript();
}
