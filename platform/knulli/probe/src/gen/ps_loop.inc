    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    context->ctr = context->gpr[5];
    }
    {
    context->gpr[3] = context->gpr[3] + 0xFFFFFFFCu;
    }
    {
    context->gpr[4] = context->gpr[4] + 0xFFFFFFFCu;
    }
label_8000450C:
    {
    const std::uint32_t ea_8000450C = context->gpr[3] + 0x00000004u;
    galaxy::load_fpr_single(context, 2u, memory, ea_8000450C, services, 0x8000450Cu);
    context->gpr[3] = ea_8000450C;
    }
    {
    const std::uint32_t ea_80004510 = context->gpr[4] + 0x00000004u;
    galaxy::load_fpr_single(context, 4u, memory, ea_80004510, services, 0x80004510u);
    context->gpr[4] = ea_80004510;
    }
    {
    galaxy::require_paired_single_mode(context, false, services, 0x80004514u);
    const std::uint32_t multiplicand_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[2], services, 0x80004514u);
    const std::uint32_t multiplicand_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[2], services, 0x80004514u);
    const std::uint32_t multiplier_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[4], services, 0x80004514u);
    const std::uint32_t multiplier_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[4], services, 0x80004514u);
    const std::uint32_t addend_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[1], services, 0x80004514u);
    const std::uint32_t addend_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[1], services, 0x80004514u);
    const galaxy::PpcFloatResult result_ps0 = galaxy::ppc_f32_ternary(galaxy::PpcFloatTernaryOperation::MultiplyAdd, multiplicand_ps0, multiplier_ps0, addend_ps0, context->fpscr);
    const galaxy::PpcFloatResult result_ps1 = galaxy::ppc_f32_ternary(galaxy::PpcFloatTernaryOperation::MultiplyAdd, multiplicand_ps1, multiplier_ps1, addend_ps1, context->fpscr);
    galaxy::ppc_commit_paired_result(context, 1u, result_ps0, result_ps1, false, false, services, 0x80004514u);
    }
    {
    galaxy::require_paired_single_mode(context, false, services, 0x80004518u);
    const std::uint32_t left_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[2], services, 0x80004518u);
    const std::uint32_t left_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[2], services, 0x80004518u);
    const std::uint32_t right_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[4], services, 0x80004518u);
    const std::uint32_t right_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[4], services, 0x80004518u);
    const galaxy::PpcFloatResult result_ps0 = galaxy::ppc_f32_binary(galaxy::PpcFloatBinaryOperation::Multiply, left_ps0, right_ps0, context->fpscr);
    const galaxy::PpcFloatResult result_ps1 = galaxy::ppc_f32_binary(galaxy::PpcFloatBinaryOperation::Multiply, left_ps1, right_ps1, context->fpscr);
    galaxy::ppc_commit_paired_result(context, 5u, result_ps0, result_ps1, false, false, services, 0x80004518u);
    }
    {
    galaxy::require_paired_single_mode(context, false, services, 0x8000451Cu);
    const std::uint32_t left_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[3], services, 0x8000451Cu);
    const std::uint32_t left_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[3], services, 0x8000451Cu);
    const std::uint32_t right_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[5], services, 0x8000451Cu);
    const std::uint32_t right_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[5], services, 0x8000451Cu);
    const galaxy::PpcFloatResult result_ps0 = galaxy::ppc_f32_binary(galaxy::PpcFloatBinaryOperation::Add, left_ps0, right_ps0, context->fpscr);
    const galaxy::PpcFloatResult result_ps1 = galaxy::ppc_f32_binary(galaxy::PpcFloatBinaryOperation::Add, left_ps1, right_ps1, context->fpscr);
    galaxy::ppc_commit_paired_result(context, 3u, result_ps0, result_ps1, false, false, services, 0x8000451Cu);
    }
    {
    galaxy::require_paired_single_mode(context, false, services, 0x80004520u);
    const std::uint32_t left_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[3], services, 0x80004520u);
    const std::uint32_t left_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[3], services, 0x80004520u);
    const std::uint32_t right_ps0 = galaxy::require_single_precision_bits(context->fpr_bits[2], services, 0x80004520u);
    const std::uint32_t right_ps1 = galaxy::require_single_precision_bits(context->ps1_bits[2], services, 0x80004520u);
    const galaxy::PpcFloatResult result_ps0 = galaxy::ppc_f32_binary(galaxy::PpcFloatBinaryOperation::Subtract, left_ps0, right_ps0, context->fpscr);
    const galaxy::PpcFloatResult result_ps1 = galaxy::ppc_f32_binary(galaxy::PpcFloatBinaryOperation::Subtract, left_ps1, right_ps1, context->fpscr);
    galaxy::ppc_commit_paired_result(context, 3u, result_ps0, result_ps1, false, false, services, 0x80004520u);
    }
    {
    if (--context->ctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; galaxy::branch_checkpoint_taken_budgeted(services, 0x80004524u, 0x8000450Cu, context, memory); } goto label_8000450C; }
    }
    {
    return;
    }
