    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    context->gpr[6] = 0x00000000u;
    }
    {
    const std::uint32_t ea_80004104 = context->gpr[1] + 0xFFFFFFF0u;
    galaxy::guest_store_u32(memory, ea_80004104, context->gpr[6], services, 0x80004104u);
    context->gpr[1] = ea_80004104;
    }
    {
    galaxy::load_fpr_single(context, 1u, memory, context->gpr[1] + 0x00000008u, services, 0x80004108u);
    }
    {
    context->gpr[1] = context->gpr[1] + 0x00000010u;
    }
    {
    context->ctr = context->gpr[5];
    }
    {
    context->gpr[3] = context->gpr[3] + 0xFFFFFFFCu;
    }
    {
    context->gpr[4] = context->gpr[4] + 0xFFFFFFFCu;
    }
label_8000411C:
    {
    const std::uint32_t ea_8000411C = context->gpr[3] + 0x00000004u;
    galaxy::load_fpr_single(context, 2u, memory, ea_8000411C, services, 0x8000411Cu);
    context->gpr[3] = ea_8000411C;
    }
    {
    const std::uint32_t ea_80004120 = context->gpr[4] + 0x00000004u;
    galaxy::load_fpr_single(context, 3u, memory, ea_80004120, services, 0x80004120u);
    context->gpr[4] = ea_80004120;
    }
    {
    galaxy::ppc_commit_scalar_single_ternary(context, 1u, galaxy::PpcFloatTernaryOperation::MultiplyAdd, 2u, 3u, 1u, false, services, 0x80004124u);
    }
    {
    if (--context->ctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; galaxy::branch_checkpoint_taken_budgeted(services, 0x80004128u, 0x8000411Cu, context, memory); } goto label_8000411C; }
    }
    {
    return;
    }
