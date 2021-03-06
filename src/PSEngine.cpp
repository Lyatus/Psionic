
#include <iostream>
#include <vector>
#include <functional>

#include "PSEngine.hpp"

using namespace std;


map<string,PSEngine::InputType, ci_less> PSEngine::to_input_type = {
	{"None", InputType::None},
	{"Up", InputType::Up},
	{"Down", InputType::Down},
	{"Left", InputType::Left},
    {"Right", InputType::Right},
    {"Action", InputType::Action},
};

std::map<string,PSEngine::ObjectMoveType, ci_less> PSEngine::to_object_move_type = {
    {"None", ObjectMoveType::None},
	{"Up", ObjectMoveType::Up},
	{"Down", ObjectMoveType::Down},
	{"Left", ObjectMoveType::Left},
    {"Right", ObjectMoveType::Right},
    {"Action", ObjectMoveType::Action},
    {"Stationary", ObjectMoveType::Stationary},
};

std::map<string,PSEngine::AbsoluteDirection, ci_less> PSEngine::to_absolute_direction ={
    {"None", AbsoluteDirection::None},
	{"Up", AbsoluteDirection::Up},
	{"Down", AbsoluteDirection::Down},
	{"Left", AbsoluteDirection::Left},
    {"Right", AbsoluteDirection::Right},
};

std::map<string,PSEngine::OperationType, ci_less> PSEngine::to_operation_type ={
    {"None", OperationType::None},
	{"Input", OperationType::Input},
	{"LoadLevel", OperationType::LoadLevel},
	{"Undo", OperationType::Undo},
    {"Restart", OperationType::Restart},
    {"LoadGame", OperationType::LoadGame},
};

std::map<string,PSEngine::ObjectDeltaType, ci_less> PSEngine::to_object_delta_type={
    {"None", ObjectDeltaType::None},
	{"Appear", ObjectDeltaType::Appear},
	{"Disappear", ObjectDeltaType::Disappear},
	{"Up", ObjectDeltaType::Up},
	{"Down", ObjectDeltaType::Down},
	{"Left", ObjectDeltaType::Left},
    {"Right", ObjectDeltaType::Right},
    {"Stationary", ObjectDeltaType::Stationary},
    {"Action", ObjectDeltaType::Action},
};

vector<CompiledGame::CommandType> PSEngine::SubturnHistory::gather_all_subturn_commands() const
{
    vector<CompiledGame::CommandType> commands;
    for( const auto& step : steps)
    {
        commands.insert(commands.end(), step.rule_applied.commands.begin(), step.rule_applied.commands.end());
    }
    return commands;
}


optional<shared_ptr<CompiledGame::PrimaryObject>> PSEngine::Cell::find_colliding_object(shared_ptr<CompiledGame::PrimaryObject> obj) const
{
    set<shared_ptr<CompiledGame::CollisionLayer>> current_col_layers;

    current_col_layers.insert(obj->collision_layer.lock());

    for(auto& pair : objects )
    {
        if( current_col_layers.count(pair.first->collision_layer.lock()) != 0)
        {
            return optional<shared_ptr<CompiledGame::PrimaryObject>> ( pair.first ) ; //collision was detected;
        }
        current_col_layers.insert(pair.first->collision_layer.lock());
    }

    return nullopt;
}

const string PSEngine::m_engine_log_cat = "engine";

PSEngine::PSEngine(shared_ptr<PSLogger> p_logger /*=nullptr*/) : m_logger(p_logger)
{
    if(m_logger == nullptr)
	{
		cout << "A Logger ptr was not passed to the engine constructor, construction a default one\n";
		m_logger = make_shared<PSLogger>(PSLogger());
	}
}

void PSEngine::load_game(const CompiledGame& p_game_to_load)
{
    m_compiled_game = p_game_to_load;

    Operation op = Operation(OperationType::LoadGame);
    op.loaded_game_title = m_compiled_game.prelude_info.title.value_or("TITLE UNKNOWN");
    m_operation_history.push_back(op);
}

void PSEngine::Load_first_level()
{
    Operation op = Operation(OperationType::LoadLevel);
    op.loaded_level = 0;
    m_operation_history.push_back(op);

    load_level_internal(0);
}

optional<vector<PSEngine::SubturnHistory>> PSEngine::receive_input(InputType p_input)
{
    m_operation_history.push_back(Operation(OperationType::Input,p_input));

    m_logger->log(PSLogger::LogType::Log, m_engine_log_cat, "Received input : " + enum_to_str(p_input,to_input_type).value_or("ERROR"));

    m_last_input = p_input;

    ObjectMoveType move_type = ObjectMoveType::None;
    switch (p_input)
    {
    case InputType::Up:
        move_type = ObjectMoveType::Up;
        break;
    case InputType::Down:
        move_type = ObjectMoveType::Down;
        break;
    case InputType::Left:
        move_type = ObjectMoveType::Left;
        break;
    case InputType::Right:
        move_type = ObjectMoveType::Right;
        break;
    case InputType::Action:
        move_type = ObjectMoveType::Action;
        break;
    default:
        detect_error("invalid input type.");
        return nullopt;
    }

    //marking player with input
    for(Cell& cell : m_current_level.cells)
    {
        for(auto& pair :cell.objects)
        {
            if(m_compiled_game.player_object.lock()->defines(pair.first))
            {
                pair.second = move_type;
            }
        }
    }

    return next_turn();
}

optional<vector<PSEngine::SubturnHistory>> PSEngine::tick(float p_delta_time)
{
    if(!m_compiled_game.prelude_info.realtime_interval.has_value())
    {
        detect_error("Trying to tick the engine but the loaded puzzlescript doesn't have realtime setup.");
        return nullopt;
    }

    float realtime_interval = m_compiled_game.prelude_info.realtime_interval.value();

    m_current_tick_time_elapsed += p_delta_time;

    optional<vector<PSEngine::SubturnHistory>> result = nullopt;

    while(m_current_tick_time_elapsed >= realtime_interval)
    {
        m_current_tick_time_elapsed -= realtime_interval;
        result = next_turn();
    }

    return result; //todo ? only yhe last one will be return if several turns happened during the same tick
}

void PSEngine::restart_level()
{
    m_operation_history.push_back(Operation(OperationType::Restart));

    load_level_internal(m_current_level.level_idx);
}

bool PSEngine::undo()
{
    m_operation_history.push_back(Operation(OperationType::Undo));

    if(m_level_state_stack.size() == 0)
    {
        return false;
    }

    m_current_level = m_level_state_stack.back();
    m_level_state_stack.pop_back();

    return true;
}


optional<vector<PSEngine::SubturnHistory>> PSEngine::next_turn()
{
    Level last_turn_save = m_current_level;
    vector<SubturnHistory> last_turn_history_save = m_turn_history;

    m_turn_history.clear();

    bool win_requested_by_command = false;
    bool keep_computing_subturn = false;
    do
    {
        keep_computing_subturn = false;

        if(next_subturn())
        {
            vector<CompiledGame::CommandType> subturn_commands = m_turn_history.back().gather_all_subturn_commands();

            for(const auto& command : subturn_commands)
            {
                if(command == CompiledGame::CommandType::Win)
                {
                    win_requested_by_command = true;
                    keep_computing_subturn = false;
                }
                else if (command == CompiledGame::CommandType::Cancel)
                {
                    m_current_level = last_turn_save;
                    m_turn_history = last_turn_history_save;
                    return nullopt; //todo shouldn't we want to know what happened that lead to a cancel ?
                }
                else if (command == CompiledGame::CommandType::Again)
                {
                    if(!win_requested_by_command)
                    {
                        keep_computing_subturn = true;
                    }
                }

            }
        }

    } while (keep_computing_subturn); //todo add an infinite loop safety

    if( m_turn_history.size() > 0)
    {
        m_level_state_stack.push_back(last_turn_save);
        print_subturns_history();
    }
    //else it means nothing happened

    if(check_win_conditions() || win_requested_by_command)
    {
        m_is_level_won = true;
    }

    return optional<vector<PSEngine::SubturnHistory>>(m_turn_history);
}

bool PSEngine::next_subturn()
{
    log("--- Subturn start ---");
    Level last_subturn_save = m_current_level;

    m_turn_history.push_back(SubturnHistory());

    for(const auto& rule : m_compiled_game.rules)
    {
        log("Processing rule : " + rule.to_string());
        apply_rule(rule);
    }

    if( !resolve_movements() )
    {
        log("could not resolve movement, cancelling the turn.");
        m_current_level = last_subturn_save;
        m_turn_history.pop_back();
        return false;
    }
    else
    {
        log("movement resolved");

        for(const auto& rule : m_compiled_game.late_rules)
        {
            log("Processing late rule : " + rule.to_string());
            apply_rule(rule);
        }

        return true;
    }
}


optional<set<PSEngine::ObjectMoveType>> PSEngine::convert_entity_rule_info_to_allowed_move_types(CompiledGame::EntityRuleInfo p_entity_rule_info, AbsoluteDirection p_rule_app_dir)
{
    if(p_entity_rule_info == CompiledGame::EntityRuleInfo::No
    || p_entity_rule_info == CompiledGame::EntityRuleInfo::None)
    {
        return nullopt;
    }

    map<AbsoluteDirection,map<CompiledGame::EntityRuleInfo,ObjectMoveType>> relative_dirs_lookup_table =
    {
        {AbsoluteDirection::Up,
            {{CompiledGame::EntityRuleInfo::RelativeUp,ObjectMoveType::Left},
            {CompiledGame::EntityRuleInfo::RelativeDown,ObjectMoveType::Right},
            {CompiledGame::EntityRuleInfo::RelativeLeft,ObjectMoveType::Down},
            {CompiledGame::EntityRuleInfo::RelativeRight,ObjectMoveType::Up},}},
        {AbsoluteDirection::Down,
            {{CompiledGame::EntityRuleInfo::RelativeUp,ObjectMoveType::Right},
            {CompiledGame::EntityRuleInfo::RelativeDown,ObjectMoveType::Left},
            {CompiledGame::EntityRuleInfo::RelativeLeft,ObjectMoveType::Up},
            {CompiledGame::EntityRuleInfo::RelativeRight,ObjectMoveType::Down},}},
        {AbsoluteDirection::Left,
            {{CompiledGame::EntityRuleInfo::RelativeUp,ObjectMoveType::Down},
            {CompiledGame::EntityRuleInfo::RelativeDown,ObjectMoveType::Up},
            {CompiledGame::EntityRuleInfo::RelativeLeft,ObjectMoveType::Right},
            {CompiledGame::EntityRuleInfo::RelativeRight,ObjectMoveType::Left},}},
        {AbsoluteDirection::Right,
            {{CompiledGame::EntityRuleInfo::RelativeUp,ObjectMoveType::Up},
            {CompiledGame::EntityRuleInfo::RelativeDown,ObjectMoveType::Down},
            {CompiledGame::EntityRuleInfo::RelativeLeft,ObjectMoveType::Left},
            {CompiledGame::EntityRuleInfo::RelativeRight,ObjectMoveType::Right},}},
    };
    set<ObjectMoveType> results;

    switch (p_entity_rule_info )
    {
    case CompiledGame::EntityRuleInfo::Stationary:
        break;
    case CompiledGame::EntityRuleInfo::Up:
        results.insert(ObjectMoveType::Up);
        break;
    case CompiledGame::EntityRuleInfo::Down:
        results.insert(ObjectMoveType::Down);
        break;
    case CompiledGame::EntityRuleInfo::Left:
        results.insert(ObjectMoveType::Left);
        break;
    case CompiledGame::EntityRuleInfo::Right:
        results.insert(ObjectMoveType::Right);
        break;
    case CompiledGame::EntityRuleInfo::Horizontal:
        results.insert(ObjectMoveType::Left);
        results.insert(ObjectMoveType::Right);
        break;
    case CompiledGame::EntityRuleInfo::Vertical:
        results.insert(ObjectMoveType::Up);
        results.insert(ObjectMoveType::Down);
        break;
    case CompiledGame::EntityRuleInfo::Orthogonal:
        results.insert(ObjectMoveType::Up);
        results.insert(ObjectMoveType::Down);
        results.insert(ObjectMoveType::Left);
        results.insert(ObjectMoveType::Right);
        break;
    case CompiledGame::EntityRuleInfo::Action:
        results.insert(ObjectMoveType::Action);
        break;
    case CompiledGame::EntityRuleInfo::Moving:
        results.insert(ObjectMoveType::Action);
        results.insert(ObjectMoveType::Up);
        results.insert(ObjectMoveType::Down);
        results.insert(ObjectMoveType::Left);
        results.insert(ObjectMoveType::Right);
        break;
    case CompiledGame::EntityRuleInfo::RelativeUp:
        results.insert(relative_dirs_lookup_table[p_rule_app_dir][CompiledGame::EntityRuleInfo::RelativeUp]);
        break;
    case CompiledGame::EntityRuleInfo::RelativeDown:
        results.insert(relative_dirs_lookup_table[p_rule_app_dir][CompiledGame::EntityRuleInfo::RelativeDown]);
        break;
    case CompiledGame::EntityRuleInfo::RelativeLeft:
        results.insert(relative_dirs_lookup_table[p_rule_app_dir][CompiledGame::EntityRuleInfo::RelativeLeft]);
        break;
    case CompiledGame::EntityRuleInfo::RelativeRight:
        results.insert(relative_dirs_lookup_table[p_rule_app_dir][CompiledGame::EntityRuleInfo::RelativeRight]);
        break;
    case CompiledGame::EntityRuleInfo::Perpendicular:
        results.insert(relative_dirs_lookup_table[p_rule_app_dir][CompiledGame::EntityRuleInfo::RelativeUp]);
        results.insert(relative_dirs_lookup_table[p_rule_app_dir][CompiledGame::EntityRuleInfo::RelativeDown]);
        break;
    case CompiledGame::EntityRuleInfo::Parallel:
        results.insert(relative_dirs_lookup_table[p_rule_app_dir][CompiledGame::EntityRuleInfo::RelativeRight]);
        results.insert(relative_dirs_lookup_table[p_rule_app_dir][CompiledGame::EntityRuleInfo::RelativeLeft]);
        break;

    default:
        break;
    }

    return optional<set<ObjectMoveType>> (results);
}

bool PSEngine::does_move_info_matches_rule(ObjectMoveType p_move_type, CompiledGame::EntityRuleInfo p_rule_info, AbsoluteDirection p_rule_dir)
{
    if(p_rule_info == CompiledGame::EntityRuleInfo::None)
    {
        return true;
    }
    else if( p_rule_info == CompiledGame::EntityRuleInfo::No)
    {
        detect_error("should not happen");
        return false;
    }
    else
    {
        optional<set<ObjectMoveType>> move_types_opt = convert_entity_rule_info_to_allowed_move_types(p_rule_info,p_rule_dir);

        if(move_types_opt.has_value())
        {
            set<PSEngine::ObjectMoveType>allowed_move_types = move_types_opt.value();

            if(allowed_move_types.size() == 0)
            {
                return p_move_type == PSEngine::ObjectMoveType::Stationary;
            }
            else
            {
                return allowed_move_types.find(p_move_type) != allowed_move_types.end();
            }
        }
        else
        {
            detect_error("should not happen");
            return false;
        }
    }
}

bool PSEngine::does_rule_cell_matches_cell(const CompiledGame::RuleCell& p_rule_cell, const PSEngine::Cell* p_cell, AbsoluteDirection p_rule_application_direction)
{
    if(!p_cell)
    {
        return false;
    }

    for(pair<shared_ptr<CompiledGame::Object>,CompiledGame::EntityRuleInfo> rule_pair : p_rule_cell.content)
    {
        bool found_object = false;

        ObjectMoveType first_found_object_move_type = ObjectMoveType::None;

        for(const auto& cell_pair : p_cell->objects)
        {
            if(rule_pair.first->defines( cell_pair.first))
            {
                if(!found_object)
                {
                    first_found_object_move_type = cell_pair.second;
                    found_object = true;
                }
                else
                {
                    //todo in some cases, such as when no delta apply to ambiguous object, it should not be an error
                    detect_error("detected multiple object that match the definition. this is ambiguous");
                    return false;
                }
            }
        }

        if(rule_pair.second == CompiledGame::EntityRuleInfo::No)
        {
            if(found_object)
            {
                return false;
            }
        }
        else if(!found_object)
        {
            return false;
        }
        else if(!does_move_info_matches_rule(first_found_object_move_type, rule_pair.second, p_rule_application_direction))
        {
            return false;
        }
    }

    return true;
}

PSEngine::RuleApplicationDelta PSEngine::compute_rule_delta(const CompiledGame::Rule& p_rule, AbsoluteDirection p_rule_app_dir,const vector<PatternMatchInformation>& p_pattern_match_infos)
{
    RuleApplicationDelta delta;

    delta.match_infos = p_pattern_match_infos;
    delta.rule_direction = p_rule_app_dir;

    for(int p = 0; p < p_rule.match_patterns.size(); ++p)
    {
        PatternMatchInformation current_pattern_match_infos = p_pattern_match_infos[p];
        auto wildcard_match_distances_iterator = current_pattern_match_infos.wildcard_match_distances.begin();

        int board_distance = -1;
        for(int i = 0; i < p_rule.match_patterns[p].cells.size(); ++i)
        {
            ++board_distance; //incrementing it here to prevent it from being incorrect (would happen if a continue statement was added somewhere without incrementing the value)

            CellDelta cell_delta;

            const auto& match_cell = p_rule.match_patterns[p].cells[i];
            const auto& result_cell = p_rule.result_patterns[p].cells[i];

            if(match_cell.is_wildcard_cell)
            {
                //the -1 is to compensate the increment at the top of the loop.
                //Because we moved one cell further in the rule but not necessarily one cell on the board since "..." could correspond to 0 cell
                board_distance += *wildcard_match_distances_iterator -1;
                ++wildcard_match_distances_iterator;
                continue;
            }

            const Cell* level_cell = get_cell_from(current_pattern_match_infos.x, current_pattern_match_infos.y, board_distance, p_rule_app_dir);

            cell_delta.x = level_cell->x;
            cell_delta.y = level_cell->y;

            for(const auto& match_content_pair : match_cell.content)
            {
                shared_ptr<CompiledGame::PrimaryObject> matched_primary_obj;
                for(const auto& level_cell_pair : level_cell->objects)
                {
                    if(match_content_pair.first->defines( level_cell_pair.first))
                    {
                        matched_primary_obj = level_cell_pair.first;
                    }
                }


                const auto& result_equivalent_pair = result_cell.content.find(match_content_pair.first);

                if(result_equivalent_pair == result_cell.content.end())
                {
                    if(match_content_pair.second != CompiledGame::EntityRuleInfo::No)
                    {
                        cell_delta.deltas.push_back(ObjectDelta(matched_primary_obj,ObjectDeltaType::Disappear));
                    }
                }
                else if(match_content_pair.second != result_equivalent_pair->second)
                {
                    shared_ptr<CompiledGame::PrimaryObject> appearing_obj = nullptr;

                    if(match_content_pair.second == CompiledGame::EntityRuleInfo::No)
                    {
                        appearing_obj = match_content_pair.first->as_primary_object();
                        if(appearing_obj)
                        {
                            cell_delta.deltas.push_back(ObjectDelta(appearing_obj,ObjectDeltaType::Appear));
                        }
                        else
                        {
                            //todo support aggregate objects ?
                            detect_error("Trying to make a non primary object appear. This is ambiguous.");
                            continue;
                        }
                    }
                    else if(result_equivalent_pair->second == CompiledGame::EntityRuleInfo::No)
                    {
                        cell_delta.deltas.push_back(ObjectDelta(matched_primary_obj,ObjectDeltaType::Disappear));
                        continue;
                    }

                    shared_ptr<CompiledGame::PrimaryObject> object_to_use_in_delta = matched_primary_obj;
                    if(!object_to_use_in_delta)
                    {
                        object_to_use_in_delta = appearing_obj;
                        if(!object_to_use_in_delta)
                        {
                            detect_error("should never happen, wasn't able to find the object for the delta");
                        }
                    }


                    if(result_equivalent_pair->second == CompiledGame::EntityRuleInfo::Moving
                    || result_equivalent_pair->second == CompiledGame::EntityRuleInfo::Parallel
                    || result_equivalent_pair->second == CompiledGame::EntityRuleInfo::Perpendicular
                    || result_equivalent_pair->second == CompiledGame::EntityRuleInfo::Vertical
                    || result_equivalent_pair->second == CompiledGame::EntityRuleInfo::Horizontal
                    || result_equivalent_pair->second == CompiledGame::EntityRuleInfo::Orthogonal)
                    {
                        //this would be an error since its ambiguous, a pass in the rule compilation should have detected that
                        detect_error("ambiguous delta");
                    }
                    else if(result_equivalent_pair->second == CompiledGame::EntityRuleInfo::None
                    || result_equivalent_pair->second == CompiledGame::EntityRuleInfo::Stationary  )
                    {
                        cell_delta.deltas.push_back(ObjectDelta(object_to_use_in_delta,ObjectDeltaType::Stationary));
                    }
                    else
                    {
                        //convert the relative movement to an absolute one if needed
                        set<ObjectMoveType> move_types = convert_entity_rule_info_to_allowed_move_types(result_equivalent_pair->second,p_rule_app_dir).value();
                        if(move_types.size() != 1)
                        {
                            detect_error("should not happen");
                        }

                        ObjectDeltaType delta_type = ObjectDeltaType::None;

                        switch (*move_types.begin())
                        {
                        case ObjectMoveType::Up:
                            delta_type = ObjectDeltaType::Up;
                            break;
                        case ObjectMoveType::Down:
                            delta_type = ObjectDeltaType::Down;
                            break;
                        case ObjectMoveType::Left:
                            delta_type = ObjectDeltaType::Left;
                            break;
                        case ObjectMoveType::Right:
                            delta_type = ObjectDeltaType::Right;
                            break;
                        case ObjectMoveType::Action:
                            delta_type = ObjectDeltaType::Action;
                            break;
                        default:
                            detect_error("should not happen");
                            break;
                        }

                        cell_delta.deltas.push_back(ObjectDelta(object_to_use_in_delta,delta_type));
                    }

                }
                else
                {
                    //no delta to add since it's all similar
                }
            }

            for(const auto& result_content_pair : result_cell.content)
            {
                const auto& match_equivalent_pair = match_cell.content.find(result_content_pair.first);
                if(match_equivalent_pair != match_cell.content.end())
                {
                    //if we found the object in the match pattern then the deltas where already handled in the for loop above
                }
                else
                {
                    shared_ptr<CompiledGame::PrimaryObject> prim_obj = result_content_pair.first->as_primary_object();

                    if(!prim_obj)//the object here must be a primary object, if not, it's ambiguous
                    {
                        detect_error("ambiguous object detected, please specify a primary object"); //this should be detected in the rule compilations
                        continue;
                    }

                    if(result_content_pair.second == CompiledGame::EntityRuleInfo::No)
                    {
                        //todo maybe we should consider this as an optional disappear


                        cell_delta.deltas.push_back(ObjectDelta(prim_obj,ObjectDeltaType::Disappear));
                    }
                    else
                    {
                        cell_delta.deltas.push_back(ObjectDelta(prim_obj,ObjectDeltaType::Appear));

                        if(result_content_pair.second == CompiledGame::EntityRuleInfo::None
                        || result_content_pair.second == CompiledGame::EntityRuleInfo::Stationary )
                        {
                            //we  consider None and Stationary to be handled by the appear delta
                        }
                        else if(result_content_pair.second == CompiledGame::EntityRuleInfo::Moving
                        || result_content_pair.second == CompiledGame::EntityRuleInfo::Parallel
                        || result_content_pair.second == CompiledGame::EntityRuleInfo::Perpendicular
                        || result_content_pair.second == CompiledGame::EntityRuleInfo::Vertical
                        || result_content_pair.second == CompiledGame::EntityRuleInfo::Horizontal
                        || result_content_pair.second == CompiledGame::EntityRuleInfo::Orthogonal)
                        {
                            //this would be an error since its ambiguous, a pass in the rule compilation should have detected that
                            detect_error("ambiguous delta");
                        }
                        else
                        {
                            //convert the relative movement to an absolute one if needed
                            set<ObjectMoveType> move_types = convert_entity_rule_info_to_allowed_move_types(result_content_pair.second,p_rule_app_dir).value();
                            if(move_types.size() != 1)
                            {
                                detect_error("should not happen");
                            }

                            ObjectDeltaType delta_type = ObjectDeltaType::None;

                            switch (*move_types.begin())
                            {
                            case ObjectMoveType::Up:
                                delta_type = ObjectDeltaType::Up;
                                break;
                            case ObjectMoveType::Down:
                                delta_type = ObjectDeltaType::Down;
                                break;
                            case ObjectMoveType::Left:
                                delta_type = ObjectDeltaType::Left;
                                break;
                            case ObjectMoveType::Right:
                                delta_type = ObjectDeltaType::Right;
                                break;
                            case ObjectMoveType::Action:
                                delta_type = ObjectDeltaType::Action;
                                break;
                            default:
                                detect_error("should not happen");
                                break;
                            }

                            cell_delta.deltas.push_back(ObjectDelta(prim_obj,delta_type));
                        }

                    }
                }

            }

            if(cell_delta.deltas.size() > 0)
            {
                delta.cell_deltas.push_back(cell_delta);
            }
        }
    }
    return delta;
}

vector<PSEngine::PatternMatchInformation> PSEngine::match_pattern(const CompiledGame::Pattern& p_pattern, AbsoluteDirection p_rule_application_direction)
{
    vector<PatternMatchInformation> match_results;

    for(const auto& cell : m_current_level.cells)
    {
        bool match_success = true;
        PatternMatchInformation current_match;
        int board_distance = 0; //cannot use i to navigate the board since it does not take into account potentials "..." offsets
        for(int i = 0; i < p_pattern.cells.size(); ++i)
        {
            const CompiledGame::RuleCell& match_cell = p_pattern.cells[i];

            if(match_cell.is_wildcard_cell)
            {
                /*if(!p_rule.result_pattern.front().cells[i].is_wildcard_cell)
                {
                    detect_error("... is not properly placed in the second part of the rule");//todo this kind of checks should be moved in the compiler
                    return;
                }
                else*/
                if(p_pattern.cells.size() <= i + 1)
                {
                    detect_error("... can not be the last element in a rule");//todo this kind of checks should be moved in the compiler
                    return vector<PatternMatchInformation>();
                }
                else if(p_pattern.cells[i+1].is_wildcard_cell)
                {
                    detect_error("... can not be followed by another ...");//todo this kind of checks should be moved in the compiler
                    return vector<PatternMatchInformation>();
                }

                const CompiledGame::RuleCell& next_match_cell = p_pattern.cells[i+1];

                int wildcard_match_distance = 0;
                bool matched_wildcard = false;

                while( Cell* board_cell =  get_cell_from(cell.x,cell.y, board_distance + wildcard_match_distance, p_rule_application_direction) )
                {
                    if(does_rule_cell_matches_cell(
                        next_match_cell,
                        board_cell,
                        p_rule_application_direction))
                    {
                        matched_wildcard = true;
                        break;
                    }

                    ++wildcard_match_distance;
                }

                if(matched_wildcard)
                {
                    //the -1 is to compensate the increment at the top of the loop.
                    //Because we moved one cell further in the rule but not necessarily one cell on the board since "..." could correspond to 0 cell
                    board_distance += wildcard_match_distance -1;

                    //also adding +1 to board_distance and i since we already checked the next rule cell (the one after the "...") was matching
                    //so we want to skip directly to the one after by simulating a step in the for loop
                    ++board_distance;
                    ++ i;

                    current_match.wildcard_match_distances.push_back(wildcard_match_distance);
                }
                else
                {
                    match_success = false;
                    break;
                }
            }
            else if(!does_rule_cell_matches_cell(
                match_cell,
                get_cell_from(cell.x,cell.y, board_distance, p_rule_application_direction),
                p_rule_application_direction))
            {
                match_success = false;
                break;
            }

            ++board_distance;
        }

        if(match_success)
        {
            current_match.x = cell.x;
            current_match.y = cell.y;
            match_results.push_back(current_match);
        }
    }

    return match_results;
}

void PSEngine::apply_rule(const CompiledGame::Rule& p_rule)
{
    RuleDelta rule_delta;

    set<AbsoluteDirection> application_directions = get_absolute_directions_from_rule_direction(p_rule.direction);

    for(auto rule_app_dir : application_directions)
    {
        std::function<void(int,vector<PatternMatchInformation>)> compute_pattern_match_combinations = [&](int rule_pattern_index, vector<PatternMatchInformation> current_match_combination){
            if(rule_pattern_index >= p_rule.match_patterns.size())
            {
                RuleApplicationDelta application_delta = compute_rule_delta(p_rule, rule_app_dir, current_match_combination);

                //do not add exactly identical deltas
                //todo this could and should probably done by the compiler (altough maybe not all equalities could be caught by the compiler)
                bool add_delta = true;
                string matched_delta_app_str = "";
                for(const auto& app_delta : rule_delta.rule_application_deltas)
                {
                    if(app_delta == application_delta)
                    {
                        add_delta = false;
                        matched_delta_app_str = enum_to_str(rule_app_dir, to_absolute_direction).value_or("error");
                        for(const auto& m : current_match_combination)
                        {
                            matched_delta_app_str += " ("+to_string(m.x)+","+to_string(m.y)+") ";
                        }

                        break;
                    }
                }

                string new_rule_match_str = enum_to_str(rule_app_dir, to_absolute_direction).value_or("error");
                for(const auto& m : current_match_combination)
                {
                    new_rule_match_str += " ("+to_string(m.x)+","+to_string(m.y)+") ";
                }

                if(add_delta)
                {
                    log("Matched the rule at "+new_rule_match_str);
                    rule_delta.rule_application_deltas.push_back(application_delta);
                }
                else
                {
                    log("Skipping match at "+new_rule_match_str+" since it equals match at "+matched_delta_app_str);

                }
            }
            else
            {
                vector<PatternMatchInformation> matched_patterns = match_pattern(p_rule.match_patterns[rule_pattern_index], rule_app_dir);

                for(const auto& match : matched_patterns)
                {
                    vector<PatternMatchInformation> new_match_combination = current_match_combination;
                    new_match_combination.push_back(match);

                    compute_pattern_match_combinations(rule_pattern_index+1,new_match_combination);
                }
            }

        };

        compute_pattern_match_combinations(0, vector<PatternMatchInformation>());
    }

    //todo check if there is collision between deltas

    if(rule_delta.rule_application_deltas.size() > 0)
    {
        rule_delta.rule_applied = p_rule;
        m_turn_history.back().steps.push_back(rule_delta);
    }

    for(const RuleApplicationDelta& delta : rule_delta.rule_application_deltas)
    {
        apply_delta(delta);
    }
}

void PSEngine::apply_delta(const RuleApplicationDelta& p_delta)
{
    for(const auto& cell_delta : p_delta.cell_deltas)
    {
        Cell* cell =  get_cell_at(cell_delta.x,cell_delta.y);

        for(const ObjectDelta& obj_delta : cell_delta.deltas)
        {
            if(!obj_delta.object)
            {
                detect_error("should never happen, there's a nullptr object in a delta");
            }
            else if(obj_delta.type == ObjectDeltaType::None)
            {
                detect_error("should not happen");
            }
            else if(obj_delta.type == ObjectDeltaType::Appear)
            {
                const auto& pair = cell->objects.find(obj_delta.object);
                if(pair == cell->objects.end() )
                {
                    cell->objects.insert(make_pair(obj_delta.object,ObjectMoveType::Stationary));
                }
                else
                {
                    string cell_coord_str = to_string(cell->x)+","+to_string(cell->y);
                    detect_error("cannot add object " +obj_delta.object->identifier+ " since there's already one in the cell ("+cell_coord_str+")");
                }
                //todo check for collisions
            }
            else if(obj_delta.type == ObjectDeltaType::Disappear)
            {
                const auto& pair = cell->objects.find(obj_delta.object);
                if(pair != cell->objects.end() )
                {
                    cell->objects.erase(pair);
                }
                else
                {
                    string cell_coord_str = to_string(cell->x)+","+to_string(cell->y);
                    detect_error("cannot delete object " +obj_delta.object->identifier+ " since it wasn't on the cell ("+cell_coord_str+")");
                }
            }
            else
            {
               //all remaining possible deltas are movement deltas
                ObjectMoveType move_type = ObjectMoveType::None;
                if(obj_delta.type ==ObjectDeltaType::Up)
                {
                    move_type = ObjectMoveType::Up;
                }
                else if(obj_delta.type ==ObjectDeltaType::Down)
                {
                    move_type = ObjectMoveType::Down;
                }
                else if(obj_delta.type ==ObjectDeltaType::Left)
                {
                    move_type = ObjectMoveType::Left;
                }
                else if(obj_delta.type ==ObjectDeltaType::Right)
                {
                    move_type = ObjectMoveType::Right;
                }
                else if(obj_delta.type == ObjectDeltaType::Action)
                {
                    move_type = ObjectMoveType::Action;
                }
                else if(obj_delta.type == ObjectDeltaType::Stationary)
                {
                    move_type = ObjectMoveType::Stationary;
                }


                const auto& pair = cell->objects.find(obj_delta.object);
                if(pair != cell->objects.end() )
                {
                    pair->second = move_type;
                }
            }
        }
    }
}

bool PSEngine::resolve_movements()
{
    return advanced_movement_resolution();
}

bool PSEngine::try_to_move_object(Cell& p_containing_cell, shared_ptr<CompiledGame::PrimaryObject> p_type_of_object_moved,RuleDelta& p_movement_deltas)
{
    std::pair<const shared_ptr<CompiledGame::PrimaryObject>,ObjectMoveType>* pair = nullptr;

    for(auto& current_pair : p_containing_cell.objects )
    {
        if(current_pair.first != p_type_of_object_moved)
        {
            continue;
        }

        pair = &current_pair;
    }

    if(pair != nullptr)
    {
        if(pair->second == ObjectMoveType::Stationary || pair->second == ObjectMoveType::Action)
        {
            return false;
        }
        else
        {
            if(pair->second == ObjectMoveType::None)
            {
                detect_error("an object move type is set to none, this should never happen");
                return false;
            }

            AbsoluteDirection dir = str_to_enum(enum_to_str(pair->second,to_object_move_type).value_or("error"), to_absolute_direction).value_or(AbsoluteDirection::None);
            if( dir == AbsoluteDirection::None)
            {
                detect_error("Could not convert ObjectMoveType to AbsoluteDirection. this should not happen");
                pair->second = ObjectMoveType::Stationary;
                return false;
            }

            if( Cell* dest_cell = get_cell_from(p_containing_cell.x,p_containing_cell.y,1,dir))
            {
                shared_ptr<CompiledGame::PrimaryObject> found_object = pair->first;

                //erase it preventively so it does not impede objects moving on this cell
                //it will be readded if the move is impossible
                p_containing_cell.objects.erase(pair->first);

                bool move_permitted = true;

                auto colliding_object = dest_cell->find_colliding_object(found_object);
                if(colliding_object.has_value())
                {
                    move_permitted = false;

                    if(try_to_move_object(*dest_cell, colliding_object.value(), p_movement_deltas))
                    {
                        move_permitted = true;
                    }
                }

                if(move_permitted)
                {
                    MovementDelta move_delta;
                    move_delta.origin_x = p_containing_cell.x;
                    move_delta.origin_y = p_containing_cell.y;
                    move_delta.destination_x = dest_cell->x;
                    move_delta.destination_y = dest_cell->y;
                    move_delta.move_direction = dir;
                    //move_delta.moved_successfully = true;
                    move_delta.object = found_object;

                    p_movement_deltas.movement_deltas.push_back(move_delta);

                    dest_cell->objects.insert(make_pair(found_object, ObjectMoveType::Stationary));
                    return true;
                }
                else
                {
                    p_containing_cell.objects.insert(make_pair(found_object, ObjectMoveType::Stationary));
                    return false;
                }

            }
            else
            {
                //movement was probably out of bounds, the movement of this object is impossible
                pair->second = ObjectMoveType::Stationary;
                return false;
            }
        }
    }

    detect_error("try to move and object but the object was not found in the cell");
    return false;
}

bool PSEngine::advanced_movement_resolution()
{
    RuleDelta movement_deltas;
    movement_deltas.is_movement_resolution = true;


    for(Cell& cell : m_current_level.cells)
    {
        vector<shared_ptr<CompiledGame::PrimaryObject>> objects_to_move;

        for(auto& pair : cell.objects )
        {
            if(pair.second == ObjectMoveType::Action)
            {
                //todo do we consider Action as a movement in the turn history ?
                pair.second = ObjectMoveType::Stationary;
            }
            else if(pair.second != ObjectMoveType::Stationary)
            {
                objects_to_move.push_back(pair.first);
            }
        }

        for(auto obj : objects_to_move)
        {
            if( !try_to_move_object(cell, obj, movement_deltas) )
            {
                //should return false only if the player can't move ?
            }
        }
    }

    m_turn_history.back().steps.push_back(movement_deltas);

    return true;
}

bool PSEngine::basic_movement_resolution()
{
    struct ObjectMoveInfo
    {
        int origin_x = -1;
        int origin_y = -1;

        int dest_x = -1;
        int dest_y = -1;

        shared_ptr<CompiledGame::PrimaryObject> obj;
    };

    vector<ObjectMoveInfo> objects_to_move;

    for(Cell& cell : m_current_level.cells)
    {
        vector<ObjectMoveInfo> current_cell_objects_to_move;

        for(auto& pair : cell.objects )
        {
            if(pair.second == ObjectMoveType::Action)
            {
                pair.second = ObjectMoveType::Stationary;
            }
            else if(pair.second != ObjectMoveType::Stationary)
            {
                if(pair.second == ObjectMoveType::None)
                {
                    detect_error("an objec move type is set to none, this should never happen");
                    continue;
                }
                ObjectMoveInfo move_info;
                move_info.origin_x = cell.x;
                move_info.origin_y = cell.y;
                move_info.obj = pair.first;

                //if some movement is invalid, the whole basic movement resolution is invalid
                if( !get_move_destination_coord(cell.x,cell.y,pair.second,move_info.dest_x, move_info.dest_y))
                {
                    return false;
                }

                current_cell_objects_to_move.push_back(move_info);
            }
        }

        for(const auto& elem :current_cell_objects_to_move)
        {
            cell.objects.erase(elem.obj);

            objects_to_move.push_back(elem);
        }
    }

    for(const auto& move_info : objects_to_move)
    {
        //check if object is not already in cell
        Cell* dest_cell = get_cell_at(move_info.dest_x,move_info.dest_y);

        if(dest_cell == nullptr)
        {
            detect_error("should not happen!");
            return false;
        }

        if(dest_cell->objects.find(move_info.obj) != dest_cell->objects.end())
        {
            //moving objects were temporarily removed from the level so only static ones should be detected here
            detect_error("this object already exist in the destination cell, basic movement resolution cannot proceed.");
            return false;
        }

        dest_cell->objects.insert(make_pair(move_info.obj, ObjectMoveType::Stationary));

    }

    //check if there's no collisions
    for(Cell& cell : m_current_level.cells)
    {
        if(cell.objects.size() <= 1)
        {
            continue;
        }

        set<shared_ptr<CompiledGame::CollisionLayer>> current_col_layers;

        for(auto& pair : cell.objects )
        {
            if( current_col_layers.count(pair.first->collision_layer.lock()) != 0)
            {
                return false; //collision was detected;
            }
            current_col_layers.insert(pair.first->collision_layer.lock());
        }
    }

    return true;
}

 bool PSEngine::check_win_conditions()
 {
    for(const CompiledGame::WinCondition& win_condition : m_compiled_game.win_conditions)
    {
        if(!check_win_condition(win_condition))
        {
            return false;
        }
    }
    return true;
 }

bool PSEngine::check_win_condition(const CompiledGame::WinCondition& p_win_condition)
{
    for(const auto & cell : m_current_level.cells)
    {

        bool on_object_found = p_win_condition.on_object == nullptr;
        bool object_found = false;
        for(const auto& cell_content : cell.objects)
        {
            if(!on_object_found && p_win_condition.on_object->defines(cell_content.first))
            {
                on_object_found = true;
            }
            if(p_win_condition.object->defines(cell_content.first))
            {
                object_found = true;
            }
        }

        switch (p_win_condition.type)
        {
        case CompiledGame::WinConditionType::Some:
            if(on_object_found && object_found)
            {
                return true;
            }
            break;
        case CompiledGame::WinConditionType::No:
            if(on_object_found && object_found)
            {
                return false;
            }
            break;
        case CompiledGame::WinConditionType::All:
            if(object_found && !on_object_found)
            {
                return false;
            }
            break;
        default:
            detect_error("should not happen");
            break;
        }
    }
    return true;
}

set<PSEngine::AbsoluteDirection> PSEngine::get_absolute_directions_from_rule_direction(CompiledGame::RuleDirection p_rule_direction)
{
    set<AbsoluteDirection> results;
    switch (p_rule_direction)
    {
    case CompiledGame::RuleDirection::Up:
        results.insert(AbsoluteDirection::Up);
        break;
    case CompiledGame::RuleDirection::Down:
        results.insert(AbsoluteDirection::Down);
        break;
    case CompiledGame::RuleDirection::Left:
        results.insert(AbsoluteDirection::Left);
        break;
    case CompiledGame::RuleDirection::Right:
        results.insert(AbsoluteDirection::Right);
        break;
    case CompiledGame::RuleDirection::Horizontal:
        results.insert(AbsoluteDirection::Left);
        results.insert(AbsoluteDirection::Right);
        break;
    case CompiledGame::RuleDirection::Vertical:
        results.insert(AbsoluteDirection::Up);
        results.insert(AbsoluteDirection::Down);
        break;
    default:
        results.insert(AbsoluteDirection::Up);
        results.insert(AbsoluteDirection::Down);
        results.insert(AbsoluteDirection::Left);
        results.insert(AbsoluteDirection::Right);
        break;
    }

    return results;
}

bool PSEngine::get_move_destination_coord(int p_origin_x, int p_origin_y, ObjectMoveType p_move_type, int& p_out_dest_x, int& p_out_dest_y)
{
    switch (p_move_type)
    {
        case ObjectMoveType::Up:
            p_origin_y -= 1;
            break;
        case ObjectMoveType::Down:
            p_origin_y += 1;
            break;
        case ObjectMoveType::Left:
            p_origin_x -= 1;
            break;
        case ObjectMoveType::Right:
            p_origin_x += 1;
            break;

        default:
            detect_error("incorrect move type specified to PSEngine::get_move_destination_cell");
            return false;
    }

    //check for out of bounds
    if(p_origin_x >= m_current_level.width || p_origin_x < 0 || p_origin_y >= m_current_level.height || p_origin_y < 0)
    {
        //detect_error("Cannot get move destination cell since the move would be out of bounds");
        return false;
    }

    p_out_dest_x = p_origin_x;
    p_out_dest_y = p_origin_y;
    return true;
}

PSEngine::Cell* PSEngine::get_cell_from(int p_origin_x, int p_origin_y, int p_distance, AbsoluteDirection p_direction)
{
    if(p_distance == 0)
    {
        return get_cell_at(p_origin_x,p_origin_y);
    }
    else
    {
        switch (p_direction)
        {
        case AbsoluteDirection::Up:
            return get_cell_from(p_origin_x,p_origin_y-1,p_distance-1,p_direction);

        case AbsoluteDirection::Down:
            return get_cell_from(p_origin_x,p_origin_y+1,p_distance-1,p_direction);

        case AbsoluteDirection::Left:
            return get_cell_from(p_origin_x-1,p_origin_y,p_distance-1,p_direction);

        case AbsoluteDirection::Right:
            return get_cell_from(p_origin_x+1,p_origin_y,p_distance-1,p_direction);


        default:
            break;
        }
    }

    detect_error("should not happen");
    return nullptr;
}

PSEngine::Cell* PSEngine::get_cell_at(int p_x, int p_y)
{
    //check for out of bounds
    if(p_x >= m_current_level.width || p_x < 0 || p_y >= m_current_level.height || p_y < 0)
    {
        //detect_error("Cannot get move destination cell since the move would be out of bounds");
        return nullptr;
    }

    return &m_current_level.cells[p_x + p_y*m_current_level.width];
}

void PSEngine::load_level_internal(int p_level_idx)
{
    if(p_level_idx >= m_compiled_game.levels.size())
    {
        detect_error("Cannot load level "+to_string(p_level_idx)+", there's only "+to_string(m_compiled_game.levels.size())+" levels.");
        return;
    }
    m_level_state_stack.clear();
    m_is_level_won = false;
    m_current_tick_time_elapsed = 0;

    CompiledGame::Level compiled_level = m_compiled_game.levels[p_level_idx];

    m_current_level = Level();

    m_current_level.level_idx = p_level_idx;
    m_current_level.width = compiled_level.width;
    m_current_level.height = compiled_level.height;


    for(int i = 0; i < compiled_level.cells.size(); ++i)
    {
        const auto& cell = compiled_level.cells[i];

        m_current_level.cells.push_back(Cell());

        m_current_level.cells.back().x = i % m_current_level.width;
        m_current_level.cells.back().y = i / m_current_level.width;

        for(weak_ptr<CompiledGame::PrimaryObject> obj : cell.objects)
        {
            m_current_level.cells.back().objects.insert(make_pair(obj.lock(),ObjectMoveType::Stationary));
        }
    }
}

void PSEngine::load_next_level()
{
    int next_level_idx = m_current_level.level_idx + 1;

    Operation op = Operation(OperationType::LoadLevel);
    op.loaded_level = next_level_idx;
    m_operation_history.push_back(op);

    load_level_internal(next_level_idx);
}

void PSEngine::load_level(int p_level_idx)
{
    Operation op = Operation(OperationType::LoadLevel);
    op.loaded_level = p_level_idx;
    m_operation_history.push_back(op);

    load_level_internal(p_level_idx);
}

bool PSEngine::is_level_won() const
{
    return m_is_level_won;
}

void PSEngine::print_game_state()
{
    string print_result_str = "\n";

    const int cell_draw_size = 2;
    const int width = m_current_level.width;
    const int height = m_current_level.height;

    vector<string> lines_to_draw;

    if(m_current_level.cells.size() != height*width)
    {
        detect_error("Cannot print level, height and width do not match with the number of cells");
    }

    auto reset_lines_to_draw = [&](){
        lines_to_draw.clear();
        for(int i = 0; i < cell_draw_size ; i++)
        {
            lines_to_draw.push_back("");
        }
    };

    auto draw_h_line = [&](){

        for(int i = 0; i < (width*(cell_draw_size+1))+1; ++i)
        {
            print_result_str += "-";
        }
        print_result_str += "\n";
    };

    reset_lines_to_draw();

    draw_h_line();

    for(int y = 0; y < height; ++y)
    {
        for(string& line :lines_to_draw)
        {
            line += "|";
        }

        for(int x = 0; x < width; ++x)
        {
            const Cell& cell = m_current_level.cells[y*width+x];

            const int draw_space = cell_draw_size*cell_draw_size;
            int draw_idx = 0;

            if(cell.objects.size() > draw_space)
            {
                detect_error("Will not be able to draw correctly level. Too many objects in a cell and too little size to draw ("+to_string(cell.objects.size())+" vs "+to_string(draw_space)+")" );
                return;
            }

            for(auto& obj : cell.objects)
            {
                string alias = get_single_char_obj_alias(obj.first->identifier);

                const int line_idx = draw_idx / cell_draw_size;

                lines_to_draw[line_idx] += alias;

                ++draw_idx;
            }

            for( ;draw_idx < draw_space; ++draw_idx)
            {
                lines_to_draw[draw_idx / cell_draw_size] += " ";
            }

            for(string& line :lines_to_draw)
            {
              line += "|";
            }
        }

        for(const string& line :lines_to_draw)
        {
            print_result_str += line + "\n";
        }

        draw_h_line();

        reset_lines_to_draw();
    }

    m_logger->log(PSLogger::LogType::Log, m_engine_log_cat, print_result_str);
}

string PSEngine::get_single_char_obj_alias(const string& p_obj_id)
{
    auto try_find_in_cache = m_single_char_obj_alias_cache.find(p_obj_id);

    if(try_find_in_cache != m_single_char_obj_alias_cache.end())
    {
        return try_find_in_cache->second;
    }

    for(const auto& obj : m_compiled_game.objects)
    {
        auto alias_obj = obj->as_alias_object();
        if(!alias_obj)
        {
            continue;
        }

        if(alias_obj->referenced_object.lock()->identifier != p_obj_id)
        {
            continue;
        }

        if(alias_obj->identifier.size() >= 2)
        {
            detect_error("Found an alias but string is more than one character, please consider adding a one char alias for " + p_obj_id + " for the level print to work.");
            return "~";
        }

        m_single_char_obj_alias_cache[p_obj_id] = alias_obj->identifier;
        return alias_obj->identifier;
    }

    detect_error("Didn't find an alias, please consider adding a one char alias for " + p_obj_id + " for the level print to work.");
    return "?";
}

void PSEngine::log(string p_log_msg, bool p_is_verbose /*= true*/)
{
    if(p_is_verbose && !m_config.verbose_logging)
    {
        return;
    }

    m_logger->log(p_is_verbose ? PSLogger::LogType::VerboseLog : PSLogger::LogType::Log, m_engine_log_cat, p_log_msg);
}

void PSEngine::detect_error(string p_error_msg)
{
    m_logger->log(PSLogger::LogType::Error, m_engine_log_cat, p_error_msg);

    if(m_config.log_operation_history_after_error)
    {
        print_operation_history();
    }
}


string PSEngine::operation_history_to_string() const
{
    string result = "Engine Operations history :\n";
    for(int i= 0; i < m_operation_history.size(); ++i)
    {
        Operation current_op = m_operation_history[i];
        result += to_string(i) + ": " + enum_to_str(current_op.operation_type, to_operation_type).value_or("ERROR") + ": ";
        switch (current_op.operation_type)
        {
        case OperationType::Input:
            result += enum_to_str(current_op.input_type, to_input_type).value_or("ERROR");
            break;
        case OperationType::LoadGame:
            result += current_op.loaded_game_title;
            break;
        case OperationType::LoadLevel:
            result += to_string(current_op.loaded_level);
            break;
        case OperationType::Undo:
        case OperationType::Restart:
            break;
        default:
            result += "ERROR this should not happen";
            break;
        }
        result += "\n";
    }
    return result;
}

void PSEngine::print_operation_history() const
{
    m_logger->log(PSLogger::LogType::Log, m_engine_log_cat, operation_history_to_string());
}

void PSEngine::print_subturns_history() const
{
    string result = "";
    for(int i = 0; i < m_turn_history.size(); ++i)
    {
        const auto& subturn = m_turn_history[i];
        result += "--- Subturn " + to_string(i+1) + "/" + to_string(m_turn_history.size()) + "---\n";
        for(const auto& rule_delta : subturn.steps)
        {
            if(rule_delta.is_movement_resolution)
            {
                result += "Movement Resolution\n";
                for(const auto& move_delta : rule_delta.movement_deltas)
                {
                    result += "\t";
                    result += (move_delta.object.get() != nullptr ? move_delta.object->identifier : "nullptr") + " ";
                    result += "moved " + enum_to_str(move_delta.move_direction,to_absolute_direction).value_or("ERROR");
                    result += " from ("+ to_string(move_delta.origin_x)+","+to_string(move_delta.origin_y);
                    result += ") to ("+to_string(move_delta.destination_x)+","+to_string(move_delta.destination_y)+")\n";
                }
            }
            else
            {
                result += rule_delta.rule_applied.to_string()+"\n";
                for(const auto& rule_app_delta : rule_delta.rule_application_deltas)
                {
                    string rule_match_str = enum_to_str(rule_app_delta.rule_direction, to_absolute_direction).value_or("error");
                    for(const auto& m : rule_app_delta.match_infos)
                    {
                        rule_match_str += " ("+to_string(m.x)+","+to_string(m.y)+") ";
                    }

                    result += "\t" + rule_match_str + "\n";
                    for(const auto& cell_delta : rule_app_delta.cell_deltas)
                    {
                        for(const auto& object_delta : cell_delta.deltas)
                        {
                            result += "\t\t" + to_string(cell_delta.x)+","+to_string(cell_delta.y)+" ";
                            result += (object_delta.object.get() != nullptr ? object_delta.object->identifier : "nullptr") + " ";
                            result += enum_to_str(object_delta.type,to_object_delta_type).value_or("ERROR") + "\n";
                        }
                    }
                }
            }
        }
    }

    m_logger->log(PSLogger::LogType::Log, m_engine_log_cat, result);
}
