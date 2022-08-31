/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#![crate_type = "proc-macro"]

use quote::quote;
use syn::parse_macro_input;
use syn::DeriveInput;

extern crate proc_macro;
use proc_macro::TokenStream;

#[proc_macro_derive(DisjointUnion)]
pub fn derive_disjoint_union(input: TokenStream) -> TokenStream {
    let code_item: DeriveInput = parse_macro_input!(input as DeriveInput);
    // Expect to be enum.
    let data_enum = match code_item.data {
        syn::Data::Enum(data_enum) => data_enum,
        _ => panic!("#[derive(DisjointUnion)] must be applied to enums only."),
    };

    let variant_idents = data_enum
        .variants
        .iter()
        .map(|variant| &variant.ident)
        .collect::<Vec<_>>();

    assert!(
        !variant_idents.is_empty(),
        "enum needs at least one variant."
    );

    let first_variant = variant_idents.first().unwrap();

    let enum_name = &code_item.ident;
    let (impl_generics, ty_generics, where_clause) = code_item.generics.split_for_impl();

    let expanded = quote! {
        impl #impl_generics AbstractDomain for #enum_name #ty_generics #where_clause {
            fn bottom() -> Self {
                #enum_name::#first_variant(AbstractDomain::bottom())
            }

            fn top() -> Self {
                #enum_name::#first_variant(AbstractDomain::top())
            }

            fn is_bottom(&self) -> bool {
                match self {
                    #( #enum_name::#variant_idents(dom) => dom.is_bottom(), )*
                }
            }

            fn is_top(&self) -> bool {
                match self {
                    #( #enum_name::#variant_idents(dom) => dom.is_top(), )*
                }
            }

            fn leq(&self, rhs: &Self) -> bool {
                if self.is_bottom() {
                    return true;
                }

                if rhs.is_bottom() {
                    return false;
                }

                if rhs.is_top() {
                    return true;
                }

                if self.is_top() {
                    return false;
                }

                return match (self, rhs) {
                    #( (#enum_name::#variant_idents(ref ldom), #enum_name::#variant_idents(rdom)) => ldom.leq(rdom), )*
                    _ => false,
                };
            }

            fn join_with(&mut self, rhs: Self) {
                match (self, rhs) {
                    #( (#enum_name::#variant_idents(ref mut ldom), #enum_name::#variant_idents(rdom)) => ldom.join_with(rdom), )*
                    (s, _) => *s = Self::top(),
                }
            }

            fn meet_with(&mut self, rhs: Self) {
                match (self, rhs) {
                    #( (#enum_name::#variant_idents(ref mut ldom), #enum_name::#variant_idents(rdom)) => ldom.meet_with(rdom), )*
                    (s, _) => *s = Self::bottom(),
                }
            }

            fn widen_with(&mut self, rhs: Self) {
                self.join_with(rhs)
            }

            fn narrow_with(&mut self, rhs: Self) {
                self.meet_with(rhs)
            }
        }
    };

    TokenStream::from(expanded)
}
