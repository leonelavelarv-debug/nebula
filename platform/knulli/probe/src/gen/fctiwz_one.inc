    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    context->fpr_bits[1] = galaxy::guest_load_u64(memory, context->gpr[3], services, 0x80004600u);
    }
    {
    const galaxy::PpcFloatResult result = galaxy::ppc_f64_to_i32_round_zero(context->fpr_bits[1], context->fpscr);
    galaxy::ppc_commit_scalar_result(context, 2u, result, false, false, services, 0x80004604u);
    }
    {
    galaxy::guest_store_u64(memory, context->gpr[4], context->fpr_bits[2], services, 0x80004608u);
    }
    {
    return;
    }
