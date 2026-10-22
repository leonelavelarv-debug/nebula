    std::uint32_t gr3 = context->gpr[3];
    std::uint32_t gr4 = context->gpr[4];
    std::uint32_t gr5 = context->gpr[5];
    std::uint32_t gctr = context->ctr;
    std::uint64_t fpr[32];
    std::uint64_t ps1[32];
    fpr[1] = context->fpr_bits[1];
    fpr[2] = context->fpr_bits[2];
    fpr[3] = context->fpr_bits[3];
    fpr[4] = context->fpr_bits[4];
    fpr[5] = context->fpr_bits[5];
    ps1[1] = context->ps1_bits[1];
    ps1[2] = context->ps1_bits[2];
    ps1[3] = context->ps1_bits[3];
    ps1[4] = context->ps1_bits[4];
    ps1[5] = context->ps1_bits[5];
    [[maybe_unused]] std::byte* const flat_guest_base = galaxy::fast::flat_base(memory);
    [[maybe_unused]] const std::byte* flat_guest_read_base = flat_guest_base;
    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    gctr = gr5;
    }
    {
    gr3 = gr3 + 0xFFFFFFFCu;
    }
    {
    gr4 = gr4 + 0xFFFFFFFCu;
    }
label_8000450C:
    {
    const std::uint32_t ea_8000450C = gr3 + 0x00000004u;
    galaxy::fast::load_fpr_single_hw_r(flat_guest_read_base, context, fpr, ps1, 2u, memory, ea_8000450C, services, 0x8000450Cu);
    gr3 = ea_8000450C;
    }
    {
    const std::uint32_t ea_80004510 = gr4 + 0x00000004u;
    galaxy::fast::load_fpr_single_hw_r(flat_guest_read_base, context, fpr, ps1, 4u, memory, ea_80004510, services, 0x80004510u);
    gr4 = ea_80004510;
    }
    {
    galaxy::fast::paired_ternary_r<galaxy::PpcFloatTernaryOperation::MultiplyAdd, galaxy::fast::PairedLane::Each>(context, fpr, ps1, 1u, 2u, 4u, 1u, false, services, 0x80004514u);
    }
    {
    galaxy::fast::paired_binary_r<galaxy::PpcFloatBinaryOperation::Multiply, galaxy::fast::PairedLane::Each>(context, fpr, ps1, 5u, 2u, 4u, false, services, 0x80004518u);
    }
    {
    galaxy::fast::paired_binary_r<galaxy::PpcFloatBinaryOperation::Add, galaxy::fast::PairedLane::Each>(context, fpr, ps1, 3u, 3u, 5u, false, services, 0x8000451Cu);
    }
    {
    galaxy::fast::paired_binary_r<galaxy::PpcFloatBinaryOperation::Subtract, galaxy::fast::PairedLane::Each>(context, fpr, ps1, 3u, 3u, 2u, false, services, 0x80004520u);
    }
    {
    if (--gctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; if (const std::uint64_t counted__ = galaxy::branch_checkpoint_due_budgeted(services); counted__ != 0u) [[unlikely]] { GALAXY_RETURN_IF_TRANSFER_PENDING(services); context->gpr[3] = gr3; context->gpr[4] = gr4; context->ctr = gctr; context->fpr_bits[1] = fpr[1]; context->fpr_bits[2] = fpr[2]; context->fpr_bits[3] = fpr[3]; context->fpr_bits[4] = fpr[4]; context->fpr_bits[5] = fpr[5]; context->ps1_bits[1] = ps1[1]; context->ps1_bits[2] = ps1[2]; context->ps1_bits[3] = ps1[3]; context->ps1_bits[4] = ps1[4]; context->ps1_bits[5] = ps1[5]; galaxy::branch_checkpoint_taken_counted(services, 0x80004524u, 0x8000450Cu, context, memory, counted__); gr3 = context->gpr[3]; gr4 = context->gpr[4]; gr5 = context->gpr[5]; gctr = context->ctr; fpr[1] = context->fpr_bits[1]; fpr[2] = context->fpr_bits[2]; fpr[3] = context->fpr_bits[3]; fpr[4] = context->fpr_bits[4]; fpr[5] = context->fpr_bits[5]; ps1[1] = context->ps1_bits[1]; ps1[2] = context->ps1_bits[2]; ps1[3] = context->ps1_bits[3]; ps1[4] = context->ps1_bits[4]; ps1[5] = context->ps1_bits[5]; GALAXY_RETURN_IF_TRANSFER_PENDING(services); } } goto label_8000450C; }
    }
    {
    { context->gpr[3] = gr3; context->gpr[4] = gr4; context->ctr = gctr; context->fpr_bits[1] = fpr[1]; context->fpr_bits[2] = fpr[2]; context->fpr_bits[3] = fpr[3]; context->fpr_bits[4] = fpr[4]; context->fpr_bits[5] = fpr[5]; context->ps1_bits[1] = ps1[1]; context->ps1_bits[2] = ps1[2]; context->ps1_bits[3] = ps1[3]; context->ps1_bits[4] = ps1[4]; context->ps1_bits[5] = ps1[5]; return; }
    }
