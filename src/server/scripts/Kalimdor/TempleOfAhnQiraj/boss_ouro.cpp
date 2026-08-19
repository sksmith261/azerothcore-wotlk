/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Cell.h"
#include "CellImpl.h" // NOTE: this import is NEEDED (even though some IDEs report it as unused)
#include "CreatureScript.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "temple_of_ahnqiraj.h"

enum Spells
{
    // Ouro
    SPELL_SWEEP                 = 26103,
    SPELL_SAND_BLAST            = 26102,
    SPELL_GROUND_RUPTURE        = 26100,
    SPELL_BERSERK               = 26615,
    SPELL_BOULDER               = 26616,
    SPELL_OURO_SUBMERGE_VISUAL  = 26063,
    SPELL_SUMMON_SANDWORM_BASE  = 26133,

    // Misc - Mounds, Ouro Spawner
    SPELL_BIRTH                 = 26586,
    SPELL_DIRTMOUND_PASSIVE     = 26092,
    SPELL_SUMMON_OURO           = 26061,
    SPELL_SUMMON_OURO_MOUNDS    = 26058,
    SPELL_QUAKE                 = 26093,
    SPELL_SUMMON_SCARABS        = 26060,
    SPELL_SUMMON_OURO_AURA      = 26642,
    SPELL_DREAM_FOG             = 24780
};

enum Misc
{
    GROUP_EMERGED               = 0,
    GROUP_PHASE_TRANSITION      = 1,

    NPC_DIRT_MOUND              = 15712,
    GO_SANDWORM_BASE            = 180795,

    DATA_OURO_HEALTH            = 0
};

struct npc_ouro_spawner : public ScriptedAI
{
    npc_ouro_spawner(Creature* creature) : ScriptedAI(creature)
    {
        Reset();
    }

    bool hasSummoned;

    void Reset() override
    {
        hasSummoned = false;
        DoCastSelf(SPELL_DIRTMOUND_PASSIVE);
    }

    void MoveInLineOfSight(Unit* who) override
    {
        // Spawn Ouro on LoS check
        if (!hasSummoned && who->IsPlayer() && me->IsWithinDistInMap(who, 40.0f) && !who->ToPlayer()->IsGameMaster())
        {
            if (InstanceScript* instance = me->GetInstanceScript())
            {
                Creature* ouro = instance->GetCreature(DATA_OURO);
                if (instance->GetBossState(DATA_OURO) != IN_PROGRESS && instance->GetBossState(DATA_OURO) != DONE && !ouro)
                {
                    DoCastSelf(SPELL_SUMMON_OURO);
                    hasSummoned = true;
                }
            }
        }

        ScriptedAI::MoveInLineOfSight(who);
    }

    void JustSummoned(Creature* creature) override
    {
        // Despawn when Ouro is spawned — but come back after a while instead
        // of never: if the attempt fails (evade or wipe in either phase),
        // the room needs a spawner again or Ouro is unobtainable until the
        // instance resets. Re-summoning is gated on the boss state above, so
        // an early respawn cannot double-spawn him.
        if (creature->GetEntry() == NPC_OURO)
        {
            creature->SetInCombatWithZone();
            me->DespawnOrUnsummon(0s, 300s);
        }
    }
};

struct boss_ouro : public BossAI
{
    boss_ouro(Creature* creature) : BossAI(creature, DATA_OURO)
    {
        me->SetCombatMovement(false);
        me->SetControlled(true, UNIT_STATE_ROOT);
    }

    bool CanAIAttack(Unit const* victim) const override
    {
        return me->IsWithinMeleeRange(victim);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType, SpellSchoolMask) override
    {
        if (me->HealthBelowPctDamaged(20, damage) && !_enraged)
        {
            DoCastSelf(SPELL_BERSERK, true);
            _enraged = true;
            scheduler.CancelGroup(GROUP_PHASE_TRANSITION);
            scheduler.Schedule(1s, [this](TaskContext context)
                {
                    if (!IsPlayerWithinMeleeRange())
                        DoSpellAttackToRandomTargetIfReady(SPELL_BOULDER);

                    context.Repeat();
                })
                .Schedule(20s, [this](TaskContext context)
                    {
                        DoCastSelf(SPELL_SUMMON_OURO_MOUNDS, true);
                        context.Repeat();
                    });
        }
    }

    void Submerge()
    {
        if (_enraged || _submerged)
            return;

        me->AttackStop();
        me->SetReactState(REACT_PASSIVE);
        me->SetUnitFlag(UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE);
        _submergeMelee = 0;
        _submerged = true;
        DoCastSelf(SPELL_OURO_SUBMERGE_VISUAL);
        scheduler.CancelGroup(GROUP_EMERGED);
        scheduler.CancelGroup(GROUP_PHASE_TRANSITION);

        if (GameObject* base = me->FindNearestGameObject(GO_SANDWORM_BASE, 10.f))
        {
            base->Use(me);
            base->DespawnOrUnsummon(6s);
        }

        DoCastSelf(SPELL_SUMMON_OURO_MOUNDS, true);

        // Reizan: scripted emerge. Upstream despawned Ouro here and handed
        // his health to a random mound via SPELL_SUMMON_OURO_AURA, trusting
        // that aura chain to re-summon him; any broken link (mound evade,
        // despawn race, dead aura) erased the boss until the instance reset.
        // He now never leaves the world — invisible, untargetable, passive
        // for the 30s submerge — and the script itself surfaces him under a
        // random raider. Threat and health carry over for free.
        me->SetVisible(false);

        scheduler.Schedule(30s, GROUP_PHASE_TRANSITION, [this](TaskContext /*context*/)
        {
            if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 100.0f, true))
                me->NearTeleportTo(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                                   me->GetOrientation());

            me->SetVisible(true);
            me->RemoveUnitFlag(UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE);
            _submerged = false;
            Emerge();
        });
    }

    void CastGroundRupture()
    {
        std::list<WorldObject*> targets;
        Acore::AllWorldObjectsInRange checker(me, 10.0f);
        Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(me, targets, checker);
        Cell::VisitObjects(me, searcher, 10.0f);

        for (WorldObject* target : targets)
        {
            if (Unit* unitTarget = target->ToUnit())
            {
                if (unitTarget->IsHostileTo(me))
                    DoCast(unitTarget, SPELL_GROUND_RUPTURE, true);
            }
        }
    }

    void SpellHitTarget(Unit* target, SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id == SPELL_SAND_BLAST && target)
        {
            me->GetThreatMgr().ModifyThreatByPercent(target, -100);
        }
    }

    void Emerge()
    {
        DoCastSelf(SPELL_BIRTH);
        DoCastSelf(SPELL_SUMMON_SANDWORM_BASE, true);
        me->SetReactState(REACT_AGGRESSIVE);
        CastGroundRupture();
        scheduler.Schedule(20s, GROUP_EMERGED, [this](TaskContext context)
                {
                    if (Unit* target = SelectTarget(SelectTargetMethod::MaxThreat, 0, 0.0f, true))
                    {
                        me->SetTarget(target->GetGUID());
                    }

                    DoCastAOE(SPELL_SAND_BLAST);

                    me->m_Events.AddEventAtOffset([this]()
                    {
                        if (Unit* victim = me->GetVictim())
                        {
                            me->SetTarget(victim->GetGUID());
                        }
                    }, 3s);

                    context.Repeat();
                })
            .Schedule(22s, GROUP_EMERGED, [this](TaskContext context)
                {
                    DoCastVictim(SPELL_SWEEP);
                    context.Repeat();
                })
            .Schedule(90s, GROUP_PHASE_TRANSITION, [this](TaskContext /*context*/)
                {
                    Submerge();
                })
            // 30s grace before the no-melee check starts: emerge's Ground
            // Rupture knocks the whole raid out of melee range of a rooted
            // boss, and starting this at 3s let him submerge ~13s after
            // spawning before anyone could path back in.
            .Schedule(30s, GROUP_PHASE_TRANSITION, [this](TaskContext context)
                {
                    if (_enraged)
                        return;

                    if (!IsPlayerWithinMeleeRange() && !_submerged)
                    {
                        if (_submergeMelee < 10)
                        {
                            _submergeMelee++;
                        }
                        else
                        {
                            Submerge();
                            _submergeMelee = 0;
                        }
                    }
                    else
                    {
                        _submergeMelee = 0;
                    }

                    if (!_submerged)
                        context.Repeat(1s);
                });
    }

    void Reset() override
    {
        instance->SetBossState(DATA_OURO, NOT_STARTED);
        scheduler.CancelAll();
        _submergeMelee = 0;
        _submerged = false;
        _enraged = false;
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        if (me->GetThreatMgr().IsThreatListEmpty())
        {
            DoCastSelf(SPELL_OURO_SUBMERGE_VISUAL);
            me->DespawnOrUnsummon(1s);
            instance->SetBossState(DATA_OURO, FAIL);
            if (GameObject* base = me->FindNearestGameObject(GO_SANDWORM_BASE, 200.f))
                base->DespawnOrUnsummon();
        }
    }

    void JustEngagedWith(Unit* who) override
    {
        Emerge();
        BossAI::JustEngagedWith(who);
    }

    void UpdateAI(uint32 diff) override
    {
        UpdateVictim();

        scheduler.Update(diff,
            std::bind(&ScriptedAI::DoMeleeAttackIfReady, this));
    }

protected:
    bool _enraged;
    uint8 _submergeMelee;
    bool _submerged;

    bool IsPlayerWithinMeleeRange() const
    {
        return me->IsWithinMeleeRange(me->GetVictim());
    }
};

struct npc_dirt_mound : ScriptedAI
{
    npc_dirt_mound(Creature* creature) : ScriptedAI(creature)
    {
        _instance = creature->GetInstanceScript();
    }

    void JustSummoned(Creature* creature) override
    {
        if (creature->GetEntry() == NPC_OURO)
        {
            creature->SetInCombatWithZone();
            creature->SetHealth(_ouroHealth);
            creature->LowerPlayerDamageReq(creature->GetMaxHealth() - creature->GetHealth());
        }
    }

    void SetData(uint32 type, uint32 data) override
    {
        if (type == DATA_OURO_HEALTH)
            _ouroHealth = data;
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        DoZoneInCombat();
        scheduler.Schedule(30s, [this](TaskContext /*context*/)
        {
            // The mound carrying the Ouro respawn aura must never despawn
            // itself: its 30s scarab timer races the aura's re-summon, and
            // losing that race erased Ouro for the rest of the lockout.
            if (me->HasAura(SPELL_SUMMON_OURO_AURA))
                return;

            DoCastSelf(SPELL_SUMMON_SCARABS, true);
            me->DespawnOrUnsummon(1s);
        })
            .Schedule(100ms, [this](TaskContext context)
        {
            ChaseNewTarget();
            context.Repeat(5s, 10s);
        });
    }

    void ChaseNewTarget()
    {
        DoResetThreatList();
        if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 200.f, true))
        {
            me->AddThreat(target, 1000000.f);
            AttackStart(target);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        scheduler.Update(diff);
    }

    void Reset() override
    {
        DoCastSelf(SPELL_DIRTMOUND_PASSIVE, true);
        DoCastSelf(SPELL_DREAM_FOG, true);
        DoCastSelf(SPELL_QUAKE, true);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        if (_instance)
        {
            _instance->SetBossState(DATA_OURO, FAIL);
        }

        if (GameObject* base = me->FindNearestGameObject(GO_SANDWORM_BASE, 200.f))
            base->DespawnOrUnsummon();

        me->DespawnOrUnsummon();
    }

protected:
    uint32 _ouroHealth;
    InstanceScript* _instance;
};

void AddSC_boss_ouro()
{
    RegisterTempleOfAhnQirajCreatureAI(npc_ouro_spawner);
    RegisterTempleOfAhnQirajCreatureAI(boss_ouro);
    RegisterTempleOfAhnQirajCreatureAI(npc_dirt_mound);
}
