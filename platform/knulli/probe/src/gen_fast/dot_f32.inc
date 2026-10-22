    std::uint32_t gr1 = context->gpr[1];
    std::uint32_t gr3 = context->gpr[3];
    std::uint32_t gr4 = context->gpr[4];
    std::uint32_t gr5 = context->gpr[5];
    std::uint32_t gr6 = context->gpr[6];
    std::uint32_t gctr = context->ctr;
    std::uint64_t fpr[32];
    std::uint64_t ps1[32];
    fpr[1] = context->fpr_bits[1];
    fpr[2] = context->fpr_bits[2];
    fpr[3] = context->fpr_bits[3];
    ps1[1] = context->ps1_bits[1];
    ps1[2] = context->ps1_bits[2];
    ps1[3] = context->ps1_bits[3];
    [[maybe_unused]] std::byte* const flat_guest_base = galaxy::fast::flat_base(memory);
    [[maybe_unused]] const std::byte* flat_guest_read_base = flat_guest_base;
    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    gr6 = 0x00000000u;
    }
    {
    const std::uint32_t ea_80004104 = gr1 + 0xFFFFFFF0u;
    galaxy::fast::store_stack<std::uint32_t>(flat_guest_base, memory, ea_80004104, gr6, services, 0x80004104u);
    gr1 = ea_80004104;
    }
    {
    galaxy::fast::load_fpr_single_hw_r(flat_guest_read_base, context, fpr, ps1, 1u, memory, gr1 + 0x00000008u, services, 0x80004108u);
    }
    {
    gr1 = gr1 + 0x00000010u;
    }
    {
    gctr = gr5;
    }
    {
    gr3 = gr3 + 0xFFFFFFFCu;
    }
    {
    gr4 = gr4 + 0xFFFFFFFCu;
    }
label_8000411C:
    {
    const std::uint32_t ea_8000411C = gr3 + 0x00000004u;
    galaxy::fast::load_fpr_single_hw_r(flat_guest_read_base, context, fpr, ps1, 2u, memory, ea_8000411C, services, 0x8000411Cu);
    gr3 = ea_8000411C;
    }
    {
    const std::uint32_t ea_80004120 = gr4 + 0x00000004u;
    galaxy::fast::load_fpr_single_hw_r(flat_guest_read_base, context, fpr, ps1, 3u, memory, ea_80004120, services, 0x80004120u);
    gr4 = ea_80004120;
    }
    {
    galaxy::fast::scalar_ternary_r<galaxy::PpcFloatTernaryOperation::MultiplyAdd, true>(context, fpr, ps1, 1u, 2u, 3u, 1u, false, services, 0x80004124u);
    }
    {
    if (--gctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; if (const std::uint64_t counted__ = galaxy::branch_checkpoint_due_budgeted(services); counted__ != 0u) [[unlikely]] { GALAXY_RETURN_IF_TRANSFER_PENDING(services); context->gpr[1] = gr1; context->gpr[3] = gr3; context->gpr[4] = gr4; context->gpr[6] = gr6; context->ctr = gctr; context->fpr_bits[1] = fpr[1]; context->fpr_bits[2] = fpr[2]; context->fpr_bits[3] = fpr[3]; context->ps1_bits[1] = ps1[1]; context->ps1_bits[2] = ps1[2]; context->ps1_bits[3] = ps1[3]; galaxy::branch_checkpoint_taken_counted(services, 0x80004128u, 0x8000411Cu, context, memory, counted__); gr1 = context->gpr[1]; gr3 = context->gpr[3]; gr4 = context->gpr[4]; gr5 = context->gpr[5]; gr6 = context->gpr[6]; gctr = context->ctr; fpr[1] = context->fpr_bits[1]; fpr[2] = context->fpr_bits[2]; fpr[3] = context->fpr_bits[3]; ps1[1] = context->ps1_bits[1]; ps1[2] = context->ps1_bits[2]; ps1[3] = context->ps1_bits[3]; GALAXY_RETURN_IF_TRANSFER_PENDING(services); } } goto label_8000411C; }
    }
    {
    { context->gpr[1] = gr1; context->gpr[3] = gr3; context->gpr[4] = gr4; context->gpr[6] = gr6; context->ctr = gctr; context->fpr_bits[1] = fpr[1]; context->fpr_bits[2] = fpr[2]; context->fpr_bits[3] = fpr[3]; context->ps1_bits[1] = ps1[1]; context->ps1_bits[2] = ps1[2]; context->ps1_bits[3] = ps1[3]; return; }
    }
