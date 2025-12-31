#include <charconv>

#include "DonationVendorComponent.h"
#include "Database.h"
#include "GeneralUtils.h"
#include "magic_enum.hpp"

DonationVendorComponent::DonationVendorComponent(Entity* parent, const int32_t componentID) : VendorComponent(parent, componentID) {
	//LoadConfigData
	m_PercentComplete = 0.0;
	m_TotalDonated = 0;
	m_TotalRemaining = 0;

        // custom attribute to calculate other values
        m_Goal = m_Parent->GetVar<int32_t>(u"donationGoal");
        if (m_Goal == 0) m_Goal = INT32_MAX;

        const auto& donationInventoryType = m_Parent->GetVar<std::u16string>(u"donationInventoryType");
        if (!donationInventoryType.empty()) {
                const auto inventoryTypeName = GeneralUtils::UTF16ToWTF8(donationInventoryType);
                // Accept either exact-case enum names or case-insensitive matches to be forgiving
                if (const auto inventoryType = magic_enum::enum_cast<eInventoryType>(inventoryTypeName, magic_enum::case_insensitive)) {
                        m_DonationReturnInventoryType = inventoryType.value();
                } else {
                        // Support numeric inventory identifiers for vanity configs that prefer integers
                        int64_t inventoryTypeId = 0;
                        const auto [ptr, ec] = std::from_chars(inventoryTypeName.data(), inventoryTypeName.data() + inventoryTypeName.size(), inventoryTypeId);
                        if (ec == std::errc() && inventoryTypeId >= 0 && inventoryTypeId <= static_cast<int64_t>(eInventoryType::ALL)) {
                                m_DonationReturnInventoryType = static_cast<eInventoryType>(inventoryTypeId);
                        }
                }
        }

        // Default to the nexus tower jawbox activity and setup settings
        m_ActivityId = m_Parent->GetVar<uint32_t>(u"activityID");
	if ((m_ActivityId == 0) || (m_ActivityId == 117)) {
		m_ActivityId = 117;
		m_PercentComplete = 1.0;
		m_TotalDonated = INT32_MAX;
		m_TotalRemaining = 0;
		m_Goal = INT32_MAX;
		return;
	}

	auto donationTotal = Database::Get()->GetDonationTotal(m_ActivityId);
	if (donationTotal) m_TotalDonated = donationTotal.value();
	m_TotalRemaining = m_Goal - m_TotalDonated;
	m_PercentComplete = m_TotalDonated/static_cast<float>(m_Goal);
}

void DonationVendorComponent::SubmitDonation(uint32_t count) {
	if (count <= 0 && ((m_TotalDonated + count) > 0)) return;
	m_TotalDonated += count;
	m_TotalRemaining = m_Goal - m_TotalDonated;
	m_PercentComplete = m_TotalDonated/static_cast<float>(m_Goal);
	m_DirtyDonationVendor = true;
}

void DonationVendorComponent::Serialize(RakNet::BitStream& outBitStream, bool bIsInitialUpdate) {
	VendorComponent::Serialize(outBitStream, bIsInitialUpdate);
	outBitStream.Write(bIsInitialUpdate || m_DirtyDonationVendor);
	if (bIsInitialUpdate || m_DirtyDonationVendor) {
		outBitStream.Write(m_PercentComplete);
		outBitStream.Write(m_TotalDonated);
		outBitStream.Write(m_TotalRemaining);
		if (!bIsInitialUpdate) m_DirtyDonationVendor = false;
	}
}
